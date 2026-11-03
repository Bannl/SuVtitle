// =============================================================================
// SuVtitle - app/bridge.cpp   (the window's page <-> the player core <-> the engine)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/bridge.cpp at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6")
// for the SuVtitle split (docs/SPLIT_PLAN.md). The player / file half of AudioPB's 8151-line bridge, its code taken
// VERBATIM by line ranges (scratchpad\s-host\tools\gen_bridge.py: each range checked at both ends, each rename
// asserted), plus the glue a one-window app needs -- see bridge.h for what changed and why. Read "the tray" as
// SuVtitle.exe, "the helper" / "the Translator" as SuVtitleEngine.exe, "the translator view/window" as THE window.
// FIX ROUND 2026-10-23 (the reviews RV2 / RV4): edited BY HAND from here on -- gen_bridge.py is the port's record,
// not to be run again. RV4-1: playerSubGet's `auto` + player_->userSubFile() before every subtitle file the viewer
// names; RV2-6: pumpTranslator's running_ guard.
// SUVTITLE LIVE (1.1.0, 2026-10-25, agent L2): the live translator's half, ported from AudioPB Translator 1.1.0 (PatchBus
// commit 50f79d6, ui/bridge.cpp) -- the section "SUVTITLE LIVE" at the end of this file, plus marked lines in the
// constructor (the strips and the live pair read), attach (the engine at the first view's attach, the player's real
// visibility), shutdown, handleWebMessage (openLive), handleTranslatorCtl (the player's Clear stays the player's),
// sendTranslatorState (the `live` object, then the Live view's own state) and pollTranslatorOnce (LINE / PART / END,
// the sources, the inputs, the levels, the gate).
// THE NATIVE VIDEO ENGINE (1.1.0, 2026-10-26, agent N2 -- docs/NATIVE_CONTRACT.md): the section of that name, before the
// LIVE one (attachNative; mpv's events -> nativeState / nativeError / nativeInfo; the page's nativeRect / nativeCmd /
// nativeCursor; the core's current item opened in mpv), plus lines marked "1.1.0 NATIVE" in the destructor, attach (the
// sync after every page message), shutdown, handleWebMessage (`ready`, the three messages), PlayerHost::sendProxy (the
// sound copy held while mpv plays), playPaths / pumpTranslator / uiTick (the sync) and handlePlayerDiag ("native").
// THE UPDATE BANNER (2026-11-03, ported from AudioPB's update.check / update.status / update.open): the section of that
// name at the end of this file, plus marked lines in the constructor (the box, app.txt's prefs), shutdown (the worker's quit
// flag), handleWebMessage (the six update.* doors, last) and pumpTranslator (the drain).
//
// JSON is produced by JsonW.h (new emitters) or by hand (the proven ones) and read by the flat jsonFind* scanners --
// or, for the editor's structured edits, by JsonR.h. Threading is std::thread + std::atomic (C++17).
// =============================================================================
#include "bridge.h"

// wrl.h before WebView2.h (WIN32_LEAN_AND_MEAN leaves MIDL_INTERFACE undefined).
#include <wrl.h>
#include <WebView2.h>
#include <shobjidl.h>               // IFileOpenDialog / IFileSaveDialog
#include <shlobj.h>                 // SHGetKnownFolderPath (the export dialog opens in Documents), SHOpenFolderAndSelectItems
#include <shellapi.h>               // U2b: FOF_* -- IFileOperation's flags (a model file to the Recycle Bin, never destroyed)

#include "TranslatorManager.h"      // the engine = a supervised SuVtitleEngine.exe
#include "DocxWriter.h"             // the transcript -> a Word document, with no dependency
#include "SubtitleWriter.h"         // WC (file mode): .srt / .vtt, header-only and pure
#include "TrFileStore.h"            // WC (file mode): the cache, the SRT naming rule, the registry
#include "TrPlayer.h"               // WE (player v2): the player's state machine (queue, jobs, ranges)
#include "SubtitleReader.h"         // WE: external subtitle files (.srt/.vtt/.ass), encoding detection
#include "JsonR.h"                  // WG: the editor's edits are structured (two texts per cue, any character)
#include "TrLaunch.h"               // WG: --play's paths checked; the TEST INSTANCE (--cpu engine, no Documents)
#include "TrAssoc.h"                // WG: "Set as default player..." (the registry, then Settings)
#include "JsonW.h"                  // structured JSON writer (all NEW emitters use it)
#include "AppConfig.h"              // SUVTITLE LIVE: app.txt's LIVEIN strips, the engine's rules for a set, the live pair
#include "MpvPlayer.h"              // 1.1.0: the native video engine (N1's libmpv wrapper; the bridge calls it, main.cpp owns it)
#include "TrTranscript.h"           // U3: the transcript's turns and its Word file's text (via TrPlayer.h too)
#include "resource.h"               // U3: SUV_VERSION_STR (the transcript's note names the version)
#include "NextaleUpdate.h"          // THE UPDATE BANNER: the site's manifest (WinHTTP, off the UI thread), the page's URL guard

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace Microsoft::WRL;

namespace patchbus {

// ----------------------------------------------------------------------------
// small string / json helpers
// ----------------------------------------------------------------------------
static std::wstring utf8ToWide(const char* s) {
    if (!s || !*s) return {};
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(n - 1, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}
static std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring jsonEscape(const std::wstring& s) {
    std::wstring o; o.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
            case L'"':  o += L"\\\""; break;
            case L'\\': o += L"\\\\"; break;
            case L'\n': o += L"\\n";  break;
            case L'\r': o += L"\\r";  break;
            case L'\t': o += L"\\t";  break;
            default: if (c < 0x20) { wchar_t b[8]; swprintf(b, 8, L"\\u%04x", (unsigned)c); o += b; }
                     else o += c;
        }
    }
    return o;
}
static bool jsonFindString(const std::wstring& j, const wchar_t* key, std::wstring& out) {
    std::wstring pat = L"\"" + std::wstring(key) + L"\"";
    size_t k = j.find(pat); if (k == std::wstring::npos) return false;
    size_t colon = j.find(L':', k + pat.size()); if (colon == std::wstring::npos) return false;
    size_t q1 = j.find(L'"', colon + 1); if (q1 == std::wstring::npos) return false;
    size_t q2 = j.find(L'"', q1 + 1);    if (q2 == std::wstring::npos) return false;
    out = j.substr(q1 + 1, q2 - q1 - 1);
    return true;
}
static bool jsonFindNumber(const std::wstring& j, const wchar_t* key, long long& out) {
    std::wstring pat = L"\"" + std::wstring(key) + L"\"";
    size_t k = j.find(pat); if (k == std::wstring::npos) return false;
    size_t colon = j.find(L':', k + pat.size()); if (colon == std::wstring::npos) return false;
    out = _wtoi64(j.c_str() + colon + 1);
    return true;
}
static bool jsonFindBool(const std::wstring& j, const wchar_t* key, bool& out) {
    std::wstring pat = L"\"" + std::wstring(key) + L"\"";
    size_t k = j.find(pat); if (k == std::wstring::npos) return false;
    size_t t = j.find(L"true", k), f = j.find(L"false", k);
    out = t < f;
    return true;
}

// THE PLAYER (wave WE): the core's hands and the folder scans' mailbox are DECLARED here, before
// the constructor that owns them (unique_ptr / shared_ptr need the complete types); their bodies
// live with the rest of the file mode, further down.
static bool trLangCode(const std::wstring& in, std::wstring& out);   // defined with the translator ctl
// THE PLAYER (wave WE): the core's hands and the folder scans' mailbox are DECLARED here, before
// the constructor that owns them (unique_ptr / shared_ptr need the complete types); their bodies
// live with the rest of the file mode, further down.
struct Bridge::PlayerHost : pbplayer::Host {
    Bridge& b;
    explicit PlayerHost(Bridge& br) : b(br) {}
    void sendFile(const pbt::trpipe::WireFile& f) override;
    void sendFocus(uint64_t wire, uint64_t ms) override;
    void sendCancel(uint64_t id) override;
    void sendProxy(const pbt::trpipe::WireProxy& p) override;
    void postBoth(const std::wstring& json) override;
    void postMain(const std::wstring& json) override;
    void postWindow(const std::wstring& json) override;
    void openWindow() override;
    void mapFolders(const std::vector<std::wstring>& folders, std::vector<std::wstring>& failed) override;
    void reloadWindow() override;
    bool identity(const std::wstring& path, uint64_t& size, uint64_t& mtime) override;
    bool readCache(const std::string& key, pbtrf::Record& r) override;
    bool peekCache(const std::string& key, pbtrf::Summary& s) override;
    void writeCache(const std::string& key, const pbtrf::Record& r) override;
    std::wstring writeSrt(const pbplayer::CueSet& set, const std::string& lang, const std::vector<pbt::trpipe::WireCue>& cues,
                          const std::string& fromKey, uint32_t fromOrd,
                          std::wstring& note, uint64_t& size, uint64_t& mtime) override;
    std::string srtWriter(const pbplayer::CueSet& set, const std::string& lang) override;
    std::wstring proxyPath(const std::string& mediaKey, uint64_t id) override;
    void deleteFile(const std::wstring& path) override;
    std::vector<pbplayer::Sidecar> sidecars(const std::wstring& folder, const std::wstring& leaf) override;
    void saveQueue(const std::string& bytes) override;
    void saveResume(const std::string& bytes) override;
    void saveAudio(const std::string& bytes) override;
    void probeTracks(uint64_t req, const std::wstring& path) override;
    void extractSub(uint64_t req, const std::wstring& path, std::shared_ptr<const pbmedia::Info> info, uint32_t track) override;
    void cancelExtract(uint64_t req) override;
    uint64_t nowMs() override;
    uint64_t epochSec() override;
    bool mpvPlays() override;          // 1.1.0 NATIVE (fix round, L2): mpv plays the current item (no sound copy then)
    // U2: an imported model failed -- the bridge persists the default and updates the menu (never calls the core back). U2b: `say`
    // = the core's word that THIS failure is to be said in a notice (once); `redone` = the file a batch redoes with the default
    void modelFailed(const std::string& kind, const std::string& path, const std::wstring& sentence, bool say,
                     const std::wstring& redone) override {
        b.onModelFailed(kind, path, sentence, say, redone);
    }
    // U2b: the models the running analyses use changed -- the menu again (its x disabled on a model in use)
    void modelsInUseChanged() override { b.postModels(); }
    // U2b (G2): a rename's Word file, rewritten IN PLACE only (never a new name, never Documents)
    bool rewriteTranscript(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& why, uint64_t& size,
                           uint64_t& mtime) override {
        return b.rewriteTranscriptDocx(set, t, why, size, mtime);
    }
    // U3 (02/11/2026, the TRANSCRIPTION): the voice pass, the voices records, the transcript's Word file, the source timecode
    void sendVoice(const pbt::trpipe::WireVoice& v) override;
    bool readVoices(const std::string& key, pbtrf::VoiceRec& v) override;
    bool peekVoices(const std::string& key, pbtrf::VoiceRec& v) override;
    void writeVoices(const std::string& key, const pbtrf::VoiceRec& v) override;
    std::wstring writeTranscript(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& note,
                                 uint64_t& size, uint64_t& mtime) override {
        return b.writeTranscriptDocx(set, t, note, size, mtime);
    }
    void probeTimecode(uint64_t req, const std::wstring& path) override;
    // FX (F2, the user's rule 02/11: one Word file per media AND sound track): which analysis' transcript a Word file -- or this
    // set's sound track's -- holds now (the registry)
    std::string docxWriter(const pbplayer::CueSet& set, const std::wstring& path) override;
};
// U2 (02/11/2026): the player's imported models -- app.txt's MODEL / MODELUSE lines as read (AppConfig.h); see "THE PLAYER'S
// OWN MODELS" further down.
struct Bridge::ModelsState { pbapp::ImportedModels m; };
// WH: probes and extractions of the film's own tracks (MediaTracks.h) run on WORKER threads -- a probe reads a few KB
// (measured 0.1-5 ms), but a header walk of a 2 GB film whose Cues cannot be used reads it all (measured 0.25-1.3 s,
// tracks_smoke), and the UI thread must never wait for a disk. Their answers wait here, like a folder scan's, until the
// UI thread drains them. Shared, never owned by `this`: a worker still running when the tray quits finds it alive; every
// worker looks at its cancel flag in each of its loops (the tray's end sets them all).
struct Bridge::TrackBox {
    std::mutex mx;
    struct Probe { uint64_t req = 0; std::shared_ptr<const pbmedia::Info> info; };
    struct Sub { uint64_t req = 0; std::shared_ptr<const pbmedia::SubResult> res; };
    struct Tc { uint64_t req = 0; pbtc::SourceTc tc; };              // U3: a file's source timecode (SourceTimecode.h)
    std::vector<Probe> probes;
    std::vector<Sub>   subs;
    std::vector<Tc>    tcs;
    std::map<uint64_t, std::shared_ptr<std::atomic<bool>>> cancels;   // an extraction in flight -> its flag
    std::atomic<bool>  quit{ false };                                 // the tray is ending: every worker stops
};
// A folder scan runs on a WORKER thread (a drop of a whole drive must not freeze both windows); its
// result waits here until the UI thread drains it (kWmTranslatorPoll -> pumpTranslator). Shared,
// never owned by `this`: a scan still running when the tray quits finds its mailbox alive.
struct Bridge::ScanBox {
    // `known` goes with `files` (read on the worker: P2); `subs` = subtitle files dropped WITH the folders,
    // applied once the scan's files are in -- to the file that then plays, not the one playing before (C8)
    struct Done {
        std::vector<std::wstring> files, subs;
        std::vector<pbplayer::Player::Known> known;
        bool truncated = false, play = false, force = false;
    };
    std::mutex        mx;
    std::vector<Done> done;
    std::atomic<bool> cancel{ false };
};
// THE UPDATE BANNER (2026-11-03): the update check's mailbox -- its worker writes the answer here and rings the window
// (kWmTranslatorPoll); the UI thread reads it in drainUpdate. Shared, never owned by `this`: a check still waiting on the
// network when the app quits finds it alive, and `quit` keeps it from ringing a window that is gone.
struct Bridge::UpdBox {
    std::mutex             mx;
    bool                   done = false;      // the worker finished (guarded by mx)
    nextale::Update::Info  info;              // its answer (guarded by mx)
    std::atomic<bool>      quit{ false };     // the app is ending
};
// WG: a cue of the subtitle FILE being edited: an id the page names it by (`i`), its text as SRT markup (<i> <b>
// <u>: what the editor shows, what is written back) and the runs the page draws.
struct TrExtCue { uint64_t i = 0, s = 0, e = 0; std::string text; std::vector<pbsubr::Run> runs; };
// WG: the subtitle FILE loaded for the current item, EDITABLE (see bridge.h).
struct Bridge::ExtDoc {
    uint64_t id = 0;
    std::wstring source;            // the file it was read from
    std::wstring path;              // where its edits go ("" = decided at the first save)
    pbsubr::Format format = pbsubr::Format::Srt;
    unsigned codepage = 0;
    std::string encName;
    bool forced = false, truncated = false;
    // review of WG #3: written back from these cues, would the file lose nothing? (an SRT/VTT read whole -- no style,
    // position, VTT setting/note/id, no cue fixed, skipped or cut). Else its edits go to a COPY, the file untouched.
    bool lossless = false;
    uint64_t srcSize = 0, srcMtime = 0;   // the source as read (or as WE last wrote it): changed by someone else = a copy
    bool bakDone = false;           // this document's one backup of the original is made
    bool edited = false;            // the user edited it (its encoding can no longer be changed: review #10)
    std::vector<TrExtCue> cues;     // start order
    uint64_t nextI = 0;
    bool dirty = false, blocked = false;
    uint64_t dirtyAt = 0;
};

// ----------------------------------------------------------------------------
// lifecycle
// ----------------------------------------------------------------------------
// SuVtitle: main.cpp builds THE Bridge only in the process that IS the app -- after the single-instance check (a
// --play forwarder never does), after the first-run import (AudioPBImport.h: what it copies is what is read here).
// AudioPB's was a namespace-scope global built in EVERY AudioPBConfig process, hence its startup() split (review of
// WG #9); the split is kept, it costs nothing.
Bridge::Bridge() {
    sink_ = std::make_shared<WebPostSink>();   // H1: the shared web-post sink (see bridge.h)
    sink_->live = true;
    translator_ = std::make_unique<pbr::TranslatorManager>();
    // W1: the manager's worker thread rings this the moment anything lands in its queues.
    // (!) hostWnd_ is read HERE, at call time, and NOT captured: this constructor runs before main.cpp has a window
    // to give us, so a captured copy would be null forever -- and every wake would silently degrade to the 1 Hz tick.
    translator_->setWake([this] {
        if (trPollPending_.exchange(true, std::memory_order_acq_rel))
            return;                       // a post is already on its way: a burst is ONE wake
        const HWND h = hostWnd_;
        if (!h || !::PostMessageW(h, kWmTranslatorPoll, 0, 0))
            trPollPending_.store(false, std::memory_order_release);   // nobody will clear it
    });
    // WE: the player, and what it was left with -- the queue and where each file was left
    // (%APPDATA%\SuVtitle\player.txt, player-resume.txt: the player's own files).
    playerHost_ = std::make_unique<PlayerHost>(*this);
    player_     = std::make_unique<pbplayer::Player>(*playerHost_);
    player_->seedShuffle(::GetTickCount64() ^ (static_cast<uint64_t>(::GetCurrentProcessId()) << 32));
    scanBox_    = std::make_shared<ScanBox>();
    trackBox_   = std::make_shared<TrackBox>();
    {
        // The pair the engine WILL run -- its own config, READ only (the engine owns it: %APPDATA%\SuVtitle\engine.txt,
        // S11; AudioPB's tray read the helper's translator.txt the same way) -- BEFORE the queue loads, so anything
        // opened before the engine's first STATE echo keys the cache with the user's pair, not en/zh (review of WE,
        // P3). The echo still decides: setPair() again on every STATE.
        std::string cfg, a = "en", b = "zh", m = "auto";
        const std::wstring cp = appDataPath(L"engine.txt");
        std::wstring na, nb;
        if (!cp.empty() && pbtrf::readFileAll(cp, cfg, 1ULL << 20) && pbplayer::pairFromTranslatorCfg(cfg, a, b, m)
            && trLangCode(utf8ToWide(a.c_str()), na) && trLangCode(utf8ToWide(b.c_str()), nb) && na != nb) {
            trLangA_ = na; trLangB_ = nb; trMode_ = utf8ToWide(m.c_str());
            player_->setPair(wideToUtf8(na), wideToUtf8(nb), m);
        }
    }
    // U2: the models the player's analyses run on (app.txt MODEL / MODELUSE) -- BEFORE the queue loads too: an analysis made
    // with an imported model has its own cache key, and the badges are read with the keys of the choice
    models_ = std::make_unique<ModelsState>();
    loadModels();
    loadLiveConfig();   // SUVTITLE LIVE: the strips (app.txt) and the LIVE pair (engine.txt, read only) -- never listening
    // THE UPDATE BANNER: the check's mailbox, and app.txt's opt-out / skipped version (nothing is asked of the site here:
    // the page's first update.check, after its `ready`, starts it)
    updBox_ = std::make_shared<UpdBox>();
    loadUpdatePrefs();
    {
        // SUVTITLE LIVE fix round (RVB-6): this run's nonce -- the wall clock in ms, hex (two runs never share one: one
        // SuVtitle at a time) -- part of every voice's name, so a voice of this run is never one of a reattached session's.
        FILETIME ft{};
        ::GetSystemTimeAsFileTime(&ft);
        const unsigned long long ms = ((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 10000ULL;
        wchar_t run[24] = {};
        ::swprintf(run, 24, L"%llx", ms);
        spkRun_ = run;
    }
    reattachLiveSession();   // SUVTITLE LIVE fix round (RVB-1): the session survives a restart until Clear
}

// The app's own start: the player's queue, positions and sound-track choices (once).
void Bridge::startup() {
    if (started_) return;
    started_ = true;
    std::string q, r, au;
    const std::wstring qp = appDataPath(L"player.txt"), rp = appDataPath(L"player-resume.txt");
    const std::wstring ap = appDataPath(L"player-audio.txt");   // WH: the user's sound-track choices
    if (!qp.empty()) pbtrf::readFileAll(qp, q, 8ULL << 20);
    if (!rp.empty()) pbtrf::readFileAll(rp, r, 8ULL << 20);
    if (!ap.empty()) pbtrf::readFileAll(ap, au, 1ULL << 20);
    player_->loadAudio(au);
    player_->load(q, r);
}

Bridge::~Bridge() {
    shutdown();
    detachNative();   // 1.1.0 NATIVE: (shutdown() returns early when the view never attached) the sink must not outlive us
    // The sink is NOT deleted here: a thread may still hold a ref and call enqueue(). shutdown() marked it dead
    // (no-op from now); the CS dies with the sink's last ref. (H1 fix, audit 2026-10-01.)
}

// %APPDATA%\SuVtitle\<file>, the folder created (AudioPB's graphFilePath named %APPDATA%\AudioPB).
std::wstring Bridge::appDataPath(const wchar_t* file) const {
    wchar_t ad[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring dir = std::wstring(ad) + L"\\SuVtitle";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + file;
}

// THE view joins: its messages (the window's handler of AudioPB's attachTranslatorView, with the dropped File
// objects), the engine (always wanted here), the player's folders mapped BEFORE the page's first navigation.
void Bridge::attach(ICoreWebView2* webview, HWND hwnd) {
    if (webview_ || !webview) return;     // once: THE view
    webview_ = webview;
    trHwnd_ = hwnd;
    startup();                            // the queue first (review of WG #9: never on top of what is opened)

    EventRegistrationToken token{};
    webview_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
                std::wstring json(raw);
                ::CoTaskMemFree(raw);
                std::wstring type;
                jsonFindString(json, L"type", type);
                if (type == L"translatorFileDrop" && trDialogBusy_) {
                    // A file dialog is up (Open, or an Export's Save): its modal loop is what
                    // delivered this. Starting a new file under it would pull the session out from
                    // under the dialog -- refused, and said.
                    postTranslatorFileNotice(L"A file dialog is open: finish it first, then drop the file again.");
                } else if (type == L"translatorFileDrop") {
                    // (!) THE PATHS COME FROM THE BROWSER, NEVER FROM THE PAGE. Dropped files -- and,
                    // since WE, dropped FOLDERS (measured, STEP0 M4: a folder arrives as a File whose
                    // ICoreWebView2File path is the folder) -- are handed over as File objects
                    // (postMessageWithAdditionalObjects); WebView2 gives us their real paths. Nothing
                    // in `json` is read as a path, and a page cannot forge such an object: a File it
                    // constructs itself is refused by the API ("not a file on the disk", probe 10/10).
                    std::vector<std::wstring> paths;
                    ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2;
                    ComPtr<ICoreWebView2ObjectCollectionView> objs;
                    if (SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&a2))) && a2
                        && SUCCEEDED(a2->get_AdditionalObjects(&objs)) && objs) {
                        UINT32 n = 0;
                        objs->get_Count(&n);
                        for (UINT32 i = 0; i < n && paths.size() < pbplayer::kQueueMax; ++i) {
                            ComPtr<IUnknown> o;
                            ComPtr<ICoreWebView2File> f;
                            LPWSTR p = nullptr;
                            if (SUCCEEDED(objs->GetValueAtIndex(i, &o)) && o && SUCCEEDED(o.As(&f)) && f
                                && SUCCEEDED(f->get_Path(&p)) && p) {
                                paths.push_back(p);
                                ::CoTaskMemFree(p);
                            }
                        }
                    }
                    std::wstring quality, target;
                    jsonFindString(json, L"quality", quality);
                    jsonFindString(json, L"target", target);   // play | queue -- a token, not a path
                    handleTranslatorFileDrop(paths, quality, target);
                } else {
                    handleWebMessage(json);
                }
                syncNative();   // 1.1.0 NATIVE: an item opened or closed by this message -> mpv opens / stops it
                return S_OK;
            }).Get(),
        &token);
    webMsgToken_ = token.value;

    // THE ENGINE. AudioPB started its helper when the user added the Translator card (and the plain player said "Add
    // the Translator card to the desk to analyse files" without one); SuVtitle IS the analysis, so it starts here,
    // once, and the supervisor respawns it if it dies. A test instance's engine runs on the CPU: a test never takes
    // the user's GPU (SPLIT_PLAN section 4). SUVTITLE LIVE: at the FIRST view's attach, whichever window's (startEngine).
    startEngine();

    // FILE MODE: a fresh view has no mapping at all. BEFORE its page's first navigation it gets the
    // proxy folder and the player's folders (the current item's and the next ones'): a mapping set
    // after a page loaded is invisible to it (measured, WC review and WE STEP0 M1). The window was
    // just created, so it is on screen.
    trMappedHosts_.clear();
    mapTranslatorProxyFolder();
    trWin_.attached();                       // on screen; its page has not said `ready` yet
    // SUVTITLE LIVE: ...unless main.cpp keeps it HIDDEN (started by --live: the Live window alone on the taskbar) --
    // then its page is told so on its `ready` (it pauses whatever it holds) and the pre-analysis waits (windowShown).
    trWin_.shown = ::IsWindowVisible(hwnd) != FALSE;
    if (player_) {
        player_->windowAttached();
        player_->windowShown(trWin_.shown);
        player_->view("file");               // SuVtitle: the page has the File mode only (S7)
    }
}

void Bridge::shutdown() {
    if (!running_.exchange(false)) return;   // never attached, or already shut down
    detachNative();                          // 1.1.0 NATIVE: mpv stops, its events no longer reach us
    // SUVTITLE LIVE: the Live view's handler (it captures `this`) goes first -- main.cpp has destroyed its window already,
    // this is the belt to those braces. The live session's journal needs nothing: every line was appended as it came.
    // Fix round: ...except what a failed write still owes it (RVB-12, one last try) and a trim's debounced LIVEIN (RVB-13).
    detachLiveView();
    if (!trJournalOwed_.empty()) writeJournalPending();
    flushLiveConfig();
    // WE: what a running job analysed and can vouch for is saved (no FDONE will come), the queue
    // and the positions are written, the current file's proxy deleted -- BEFORE the engine goes.
    if (extDoc_ && extDoc_->dirty) { extDoc_->blocked = false; saveExtDoc(); }   // WG: an edited subtitle file first
    if (player_) player_->shutdown();
    if (scanBox_) scanBox_->cancel.store(true);
    if (trackBox_) {                                   // WH: probes and extractions still running stop at their next check
        trackBox_->quit.store(true);
        std::lock_guard<std::mutex> lk(trackBox_->mx);
        for (auto& c : trackBox_->cancels) c.second->store(true);
    }
    if (translator_) translator_->stop();  // QUIT -> the engine stops its two model servers and exits
    if (updBox_) updBox_->quit.store(true);   // THE UPDATE BANNER: a check still on the network rings no window now
    // Unsubscribe the WebMessageReceived handler (it captures `this`) before dropping the view.
    if (webview_ && webMsgToken_) {
        EventRegistrationToken t; t.value = webMsgToken_;
        webview_->remove_WebMessageReceived(t);
        webMsgToken_ = 0;
    }
    webview_ = nullptr;
    trHwnd_ = nullptr;
    trMappedHosts_.clear();   // the mappings die with the view
    trWin_.detached();
    if (player_) { player_->windowDetached(); player_->windowShown(false); }
    // H1: mark the shared post sink dead LAST: a post after this is a safe no-op.
    if (sink_) {
        ::EnterCriticalSection(&sink_->cs);
        sink_->live = false;
        sink_->hwnd = nullptr;
        ::LeaveCriticalSection(&sink_->cs);
    }
}

// ----------------------------------------------------------------------------
// post to web (marshal to UI thread)
// ----------------------------------------------------------------------------
void Bridge::postToWeb(const std::wstring& json) {
    if (hostWnd_ && ::GetWindowThreadProcessId(hostWnd_, nullptr) == ::GetCurrentThreadId()) {
        if (webview_) webview_->PostWebMessageAsJson(json.c_str());
        return;
    }
    // Off the UI thread: route through the shared sink (survives teardown -- see bridge.h).
    if (sink_) sink_->enqueue(json);
}

void Bridge::drainWebQueue() {
    std::wstring batch;
    if (sink_) {
        ::EnterCriticalSection(&sink_->cs);
        batch.swap(sink_->pending);
        ::LeaveCriticalSection(&sink_->cs);
    }
    if (batch.empty() || !webview_) return;
    size_t start = 0;
    while (start < batch.size()) {
        const size_t nl = batch.find(L'\n', start);
        const std::wstring one = batch.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        if (!one.empty()) webview_->PostWebMessageAsJson(one.c_str());
        if (nl == std::wstring::npos) break;
        start = nl + 1;
    }
}

// ----------------------------------------------------------------------------
// inbound from the page
// ----------------------------------------------------------------------------
// AudioPB's handleWebMessage served two pages (the desk and the translator window): SuVtitle keeps the branches of
// the translator window's FILE MODE, verbatim, plus the engine's translatorCtl. Gone with the desk and the LIVE
// translator (AudioPB keeps them): addTranslator / removeTranslator, openTranslatorWindow / openPlayer, the live
// session's translatorExport, mainFileDrop, and every message of the desk. An unknown type is ignored, never
// answered -- as before.
void Bridge::handleWebMessage(const std::wstring& json) {
    std::wstring type;
    if (!jsonFindString(json, L"type", type)) return;

    if (type == L"ready") {
        // The page (re)loaded -- its first load, or a reload for a new folder: it gets the engine's state AND the
        // whole player back (the session, every cue, the queue, the external subtitle, the window's own state).
        sendTranslatorState(/*force=*/true);
        // 1.1.0 NATIVE: the video engine, and what mpv holds now -- BEFORE the player's replay (the page must know which
        // backend plays the item the replay names; a film mpv plays on under a reloaded page is adopted, not reopened)
        replayNative();
        // ITS page is live from now on: a pending "show the File tab" reaches it once, and is forgotten (C1).
        trWin_.ready();
        postTranslatorWindowState();
        if (player_) {
            player_->windowReady();
            player_->view("file");   // SuVtitle: whatever the page says, there is no other view (S7)
            postModels();            // U2: the models menu (the imported files, which are there, the choice of each kind)
        }
        return;
    }
    // ---- the engine: the PAIR, the direction, the quality preset (W2) ----------------------------------------
    if (type == L"translatorCtl") {
        std::wstring mode, preset, langA, langB;
        jsonFindString(json, L"mode", mode);
        jsonFindString(json, L"preset", preset);
        // W2: the window's language bar steers the PAIR from here. Absent = unchanged, which is why they are read
        // into empty strings and never defaulted.
        jsonFindString(json, L"langA", langA);
        jsonFindString(json, L"langB", langB);
        bool clear = false; jsonFindBool(json, L"clear", clear);
        // The window's pin toggle ships as translatorCtl{alwaysOnTop} (and as translatorTopmost{value}, below):
        // both spellings accepted so neither surface owns a dead button.
        bool topOn = false;
        if (json.find(L"\"alwaysOnTop\"") != std::wstring::npos && jsonFindBool(json, L"alwaysOnTop", topOn) && hostWnd_)
            ::PostMessageW(hostWnd_, kWmTranslatorWindow, 1, topOn ? 1 : 0);
        handleTranslatorCtl(mode, preset, clear, langA, langB);
        return;
    }
    if (type == L"translatorTopmost") {
        bool on = false; jsonFindBool(json, L"value", on);
        if (hostWnd_) ::PostMessageW(hostWnd_, kWmTranslatorWindow, 1, on ? 1 : 0);
        return;
    }
    if (type == L"translatorReplay") {   // explicit "give me the state again" (AudioPB also replayed the live log)
        sendTranslatorState(/*force=*/true);
        return;
    }
    // SUVTITLE LIVE (1.1.0): the player's Live button (LIVE_PLAN 3.3) -- the windows are main.cpp's, the bridge asks.
    if (type == L"openLive") {
        if (hostWnd_) ::PostMessageW(hostWnd_, kWmTranslatorWindow, kWinOpenLive, 0);
        return;
    }
    // 1.1.0 NATIVE: the picture's place, mpv's transport, the cursor safety net (docs/NATIVE_CONTRACT.md) -- numbers and
    // tokens only, read by JsonR (their keys are on D1's closed list); never a path: mpv plays what syncNative opened
    if (type == L"nativeRect" || type == L"nativeCmd" || type == L"nativeCursor") { handleNativeMessage(type, json); return; }
    // FILE MODE (wave WC) + THE PLAYER (wave WE). Every field read here is a closed-vocabulary
    // token, a number, or an id the bridge minted -- no path, no free text ever comes from a page.
    // The state machine is player_ (TrPlayer.h); this only translates messages into its calls.
    if (type == L"translatorFileOpen" || type == L"playerOpen" || type == L"playerAdd") {
        // translatorFileOpen = v1's button = playerOpen{files}
        std::wstring what, q;
        jsonFindString(json, L"what", what);
        jsonFindString(json, L"quality", q);
        handlePlayerOpen(what == L"folder" ? L"folder" : L"files", type != L"playerAdd", q);
        return;
    }
    if (type == L"translatorFileDrop") {
        // Honoured ONLY by the view's own handler, where the dropped File objects are (attach). A copy
        // reaching here carries no file: ignored on purpose.
        return;
    }
    if (!player_) return;
    if (type == L"translatorFileAnalyse") {
        bool force = false; jsonFindBool(json, L"force", force);
        std::wstring q;
        if (jsonFindString(json, L"quality", q)) player_->setQuality(wideToUtf8(q));
        player_->analyse(force);
        return;
    }
    if (type == L"translatorFileCancel") {
        long long id = 0; jsonFindNumber(json, L"jobId", id);
        player_->cancel(id > 0 ? (uint64_t)id : 0);
        return;
    }
    // U1 (02/11/2026): the queue panel's "Translate all" (every file of the list not complete yet, one at a time, the
    // playing one first) and "Cancel all" -- a quality token, nothing else (docs/NATIVE_CONTRACT.md section 7)
    if (type == L"translatorFileAnalyseAll") {
        std::wstring q;
        if (jsonFindString(json, L"quality", q)) player_->setQuality(wideToUtf8(q));
        player_->analyseAll();
        return;
    }
    if (type == L"translatorFileCancelAll") { player_->cancelAll(); return; }
    // U3 (02/11/2026, the TRANSCRIPTION -- docs/NATIVE_CONTRACT.md section 9): Transcribe (force = Re-transcribe), its own
    // Cancel (the session's generation), "Transcribe all" (a quality token), the tab's Save .docx..., a speaker's name. The
    // only free text is the NAME the user typed: read by JsonR (any character), checked by the core (TrTranscript.h nameOk:
    // trimmed, <= 64 bytes of UTF-8, no control character) -- never a path, never markup.
    if (type == L"translatorTranscribe") {
        bool force = false; jsonFindBool(json, L"force", force);
        std::wstring q;
        if (jsonFindString(json, L"quality", q)) player_->setQuality(wideToUtf8(q));
        player_->transcribe(force);
        return;
    }
    if (type == L"translatorTranscriptCancel") {
        long long id = 0; jsonFindNumber(json, L"jobId", id);
        player_->transcriptCancel(id > 0 ? (uint64_t)id : 0);
        return;
    }
    if (type == L"translatorTranscribeAll") {
        std::wstring q;
        if (jsonFindString(json, L"quality", q)) player_->setQuality(wideToUtf8(q));
        player_->transcribeAll();
        return;
    }
    if (type == L"translatorTranscriptSave") { handleTranscriptSave(); return; }
    if (type == L"translatorSpeakerName") {
        // (FX F8: `vrev` = the speakers' numbering the name was typed against -- a number, required: the core refuses a stale one)
        pbjson::Value v;
        uint64_t gen = 0, id = 0, vrev = 0;
        if (json.size() > 4096 || !pbjson::parse(json, v, 4096) || !v.isObj() || !v.u64(L"jobId", gen) || !v.u64(L"id", id)
            || id > 64 || !v.u64(L"vrev", vrev)) return;
        const pbjson::Value* lv = v.get(L"label");
        if (!lv || lv->kind != pbjson::Value::Kind::Str) return;
        std::wstring why;
        if (!player_->renameSpeaker(gen, vrev, static_cast<uint32_t>(id), wideToUtf8(lv->s), &why) && !why.empty())
            postTranslatorFileNotice(why, true);
        return;
    }
    // U2 (02/11/2026): "Import model..." -- the dialog runs HERE; a pick names a kind ("asr" | "mt") and an id the bridge
    // minted (0 = SuVtitle's default model), never a file (docs/NATIVE_CONTRACT.md section 8)
    if (type == L"translatorModelImport") { handleModelImport(); return; }
    if (type == L"translatorModelPick") {
        std::wstring kind;
        long long id = -1;
        jsonFindString(json, L"kind", kind);
        jsonFindNumber(json, L"id", id);
        handleModelPick(kind, id);
        return;
    }
    // U2b (02/11/2026, the user: remove a model from the menu, "et demande si on supprime le model en plus"): a kind, an id the
    // bridge minted and a flag -- the PATH is resolved from the bridge's own list (app.txt), never received; recycle = the
    // file to the Recycle Bin too (docs/NATIVE_CONTRACT.md section 10)
    if (type == L"translatorModelRemove") {
        std::wstring kind;
        long long id = -1;
        bool recycle = false;
        jsonFindString(json, L"kind", kind);
        jsonFindNumber(json, L"id", id);
        if (json.find(L"\"recycle\"") != std::wstring::npos) jsonFindBool(json, L"recycle", recycle);
        handleModelRemove(kind, id, recycle);
        return;
    }
    if (type == L"translatorFileClose")  { player_->close(); return; }
    if (type == L"translatorFileReplay") { player_->replay(); return; }
    if (type == L"translatorFileExport") {
        std::wstring fmt, which;
        jsonFindString(json, L"format", fmt);
        jsonFindString(json, L"which", which);
        handleTranslatorFileExport(fmt, which);
        return;
    }
    if (type == L"translatorFilePlayFailed") {
        // Named by the SESSION (seq), not the job: a re-analysis started while the page was still
        // finding out that it cannot play the file must not make its request look stale.
        long long seq = 0; jsonFindNumber(json, L"seq", seq);
        player_->playFailed(seq > 0 ? (uint64_t)seq : 0);
        return;
    }
    if (type == L"translatorFileReveal") {
        std::wstring what; jsonFindString(json, L"what", what);
        handleTranslatorFileReveal(what);
        return;
    }
    if (type == L"playerQueue") {
        std::wstring op; jsonFindString(json, L"op", op);
        long long id = 0, to = 0, ms = 0;
        jsonFindNumber(json, L"id", id);
        jsonFindNumber(json, L"to", to);
        jsonFindNumber(json, L"ms", ms);
        player_->queueOp(wideToUtf8(op), id > 0 ? (uint64_t)id : 0, to, ms > 0 ? (uint64_t)ms : 0);
        return;
    }
    if (type == L"playerSet") {
        // Each field only when the page sent it: one click changes one thing. (U1: 1.0.x's `translate` is no longer read --
        // a file is analysed when Translate is pressed on it, translatorFileAnalyse, or by translatorFileAnalyseAll.)
        bool b = false;
        std::wstring rep;
        if (jsonFindString(json, L"repeat", rep)) player_->setRepeat(pbplayer::repeatFrom(wideToUtf8(rep)));
        if (json.find(L"\"shuffle\"") != std::wstring::npos && jsonFindBool(json, L"shuffle", b)) player_->setShuffle(b);
        // WH (D5): the sound track -- the user's choice, or `auto` (the page could not play the tray's: not persisted).
        // A number the core checks against the item's own track list; nothing else is read from it.
        long long ord = -1;
        if (json.find(L"\"audioOrd\"") != std::wstring::npos && jsonFindNumber(json, L"audioOrd", ord)) {
            bool autoPick = false;
            if (json.find(L"\"auto\"") != std::wstring::npos) jsonFindBool(json, L"auto", autoPick);
            if (ord >= 0 && ord < static_cast<long long>(pbmedia::kMaxTracks)) player_->setAudio(static_cast<int>(ord), autoPick);
        }
        return;
    }
    if (type == L"playerPos") {
        long long gen = 0, ms = 0, dur = 0;
        bool playing = false;
        std::wstring reason;
        jsonFindNumber(json, L"gen", gen);
        jsonFindNumber(json, L"ms", ms);
        jsonFindNumber(json, L"durMs", dur);
        jsonFindBool(json, L"playing", playing);
        jsonFindString(json, L"reason", reason);
        player_->pos(gen > 0 ? (uint64_t)gen : 0, ms > 0 ? (uint64_t)ms : 0, dur > 0 ? (uint64_t)dur : 0, playing, wideToUtf8(reason));
        return;
    }
    if (type == L"playerDiag") { handlePlayerDiag(json); return; }   // THE FREEZE WATCH (25/10): a freeze -> player.log
    if (type == L"playerView") {
        // AudioPB: the Live | File tabs (the pre-analysis runs on File only, D14). SuVtitle has File only: the core's
        // view stays "file" whatever a page says (attach / ready set it).
        return;
    }
    if (type == L"playerSubLoad") { handlePlayerSubLoad(); return; }
    if (type == L"playerSubGet") {
        // An id the bridge minted (a sidecar, or a file loaded earlier) + an optional code page the
        // user forced in the CC panel. The PATH is looked up here, never received.
        long long id = 0, enc = 0;
        bool isAuto = false;
        jsonFindNumber(json, L"id", id);
        jsonFindNumber(json, L"enc", enc);
        jsonFindBool(json, L"auto", isAuto);
        std::wstring path;
        const uint64_t uid = id > 0 ? (uint64_t)id : 0;
        // SuVtitle fix round (RV4-1, ADDENDUM 2): the page's AUTOMATIC choice is ignored -- no answer -- once the viewer
        // named or loaded a subtitle for this item; a request without `auto` is his, always honoured (and marks it)
        if (!player_->acceptSubGet(uid, isAuto)) return;
        // WH (WHP-1): id 0 = the viewer abandoned the pending request -- the embedded extraction (if any) is cancelled and
        // nothing is answered
        if (!uid) { player_->cancelSubRequest(); return; }
        // WH (D7): a subtitle track INSIDE the video -- extracted on a worker by the core's request (always UTF-8: an
        // encoding forced in the CC panel means nothing for it); its text comes back as playerSubs{embedded:true}
        if (player_->subGet(uid)) return;
        const unsigned cp = (enc > 0 && pbsubr::isOfferedEncoding((unsigned)enc)) ? (unsigned)enc : 0u;
        // WG (review #10): the file the user EDITED is not read again in another encoding -- it would drop his edits
        // (or, once written back as UTF-8, misread them); what he sees stays what he edited
        if (extDoc_ && extDoc_->id == uid && extDoc_->edited) {
            postTranslatorFileNotice(L"This subtitle file was edited: it is not read again in another encoding "
                                     L"(your edits are kept). Save as\x2026 makes a copy.");
            return;
        }
        if (player_->sidecarPath(uid, path)) loadExtSub(path, uid, cp);
        else if (extSubPaths_.count(uid)) loadExtSub(extSubPaths_[uid], uid, cp);
        return;
    }
    if (type == L"translatorFullscreen") {
        bool on = false; jsonFindBool(json, L"on", on);
        if (hostWnd_) ::PostMessageW(hostWnd_, kWmTranslatorWindow, 2, on ? 1 : 0);
        return;
    }
    // WG: THE SUBTITLE EDITOR. An edit = "remove these cues, add these" (the page computes it), checked and applied by
    // the core (a SuVtitle set) or here (a subtitle file). Structured: read by JsonR.h, never by jsonFind*.
    if (type == L"subEdit") { handleSubEdit(json); return; }
    if (type == L"playerSetDefault") { handleSetDefaultPlayer(); return; }   // WG: SuVtitle as a Windows player
    if (type == L"subSaveAs") {
        long long id = 0; jsonFindNumber(json, L"id", id);
        handleSubSaveAs(id > 0 ? (uint64_t)id : 0);
        return;
    }
    // THE UPDATE BANNER (2026-11-03): AudioPB's three doors and the banner's own -- no member of theirs names a URL (D1): what
    // is opened is what the APP built (openUpdatePage: pageUrl), never an address the page hands back; update.open reads
    // NOTHING from the page.
    if (type == L"update.check")  { startUpdateCheck(); postUpdateStatus(); return; }   // (the site: once per start)
    if (type == L"update.open")   { openUpdatePage(); return; }                         // (SuVtitle's page: the app's address)
    if (type == L"update.later")  { updLater_ = true; postUpdateStatus(); return; }     // (this run: a reload stays told)
    if (type == L"update.skip") {
        if (!updVersion_.empty() && updSkip_ != updVersion_) { updSkip_ = updVersion_; saveUpdatePrefs(); }
        postUpdateStatus();
        return;
    }
    // (review 03/11, S5) "Offer it again" -- the Updates dialog: a skip pressed by mistake is undone (UPDATESKIP cleared), and the
    // banner comes back now (Later with it: the user asked to see it)
    if (type == L"update.unskip") {
        if (!updSkip_.empty()) { updSkip_.clear(); saveUpdatePrefs(); }
        updLater_ = false;
        postUpdateStatus();
        return;
    }
    if (type == L"update.enable") {
        bool on = false;
        if (!jsonFindBool(json, L"on", on)) { postUpdateStatus(); return; }   // (no `on`: nothing is changed)
        if (on != updEnabled_) { updEnabled_ = on; saveUpdatePrefs(); }
        if (on) startUpdateCheck();   // (turned on: asked now, if this run has not asked yet)
        postUpdateStatus();
        return;
    }
}

// A language code as the wire wants it: two ASCII letters, lower case -- or one of the spellings
// below, mapped to its canonical code. Anything else is REFUSED here rather than forwarded,
// because the helper picks its whisper prompt and its MT prompt from this very string: an
// "EN-GB" or a stray space would not fail loudly, it would quietly translate into nothing.
//
// (!) NOT "TWO LETTERS OR NOTHING" ANY MORE (wave WA). "zh-tw" (Traditional Chinese, Taiwan) is
// five characters, and it is not a dialect of the rule: it is the same Mandarin in another
// SCRIPT, so it must never be shortened to "zh" -- that would hand a Taiwanese session a
// mainland translation without one error anywhere. Exact spellings only, never a prefix.
// TWIN of translator/TrEngine.cpp's normalizeLangCode (the helper's door) -- the two
// vocabularies must stay row-for-row the same, or the tray mirrors a pair the helper refused.
static bool trLangCode(const std::wstring& in, std::wstring& out) {
    std::wstring v;
    for (wchar_t c : in) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        v += c;
    }
    struct Alias { const wchar_t* in; const wchar_t* out; };
    static const Alias kAlias[] = {
        { L"eng", L"en" }, { L"zho", L"zh" }, { L"chi", L"zh" }, { L"cmn", L"zh" }, { L"yue", L"yue" },
        { L"zh-tw", L"zh-tw" }, { L"zh_tw", L"zh-tw" }, { L"zh-hant", L"zh-tw" },
        { L"zh_hant", L"zh-tw" }, { L"zh-hk", L"zh-tw" }, { L"zh-mo", L"zh-tw" }, { L"cht", L"zh-tw" },
    };
    for (const auto& a : kAlias) if (v == a.in) { out = a.out; return true; }
    if (v.size() != 2) return false;
    for (wchar_t c : v) if (c < L'a' || c > L'z') return false;
    out = v;
    return true;
}

// ============================================================================
// THE ENGINE'S CONTROLS AND STATE (AudioPB: the Translator card's). The PAIR, the direction and the quality preset
// are the ENGINE's facts -- it persists them (engine.txt, S11) and echoes what is RUNNING in every STATE; the app asks
// with a CTL and mirrors the echo. One owner per fact.
// ============================================================================
void Bridge::handleTranslatorCtl(const std::wstring& mode, const std::wstring& preset,
                                 bool clear, const std::wstring& langA,
                                 const std::wstring& langB) {
    if (!translator_) return;
    auto oneOf = [](const std::wstring& v, std::initializer_list<const wchar_t*> ok) {
        for (const wchar_t* k : ok) if (v == k) return true;
        return false;
    };
    // W2: a DIRECTION over the pair, not two hard-coded languages. en2zh/zh2en are still read
    // and mean exactly what they used to when the pair was en/zh.
    std::wstring m = mode;
    if      (m == L"en2zh") m = L"a2b";
    else if (m == L"zh2en") m = L"b2a";
    if (oneOf(m, { L"auto", L"a2b", L"b2a" })) {
        trMode_ = m;
        translator_->setMode(wideToUtf8(m));
    }
    // NOT mirrored into trPreset_: that one is the RESOLVED engine and belongs to the engine's
    // STATE. What the user asked for lives in the manager and travels as presetReq.
    if (oneOf(preset, { L"auto", L"quality", L"balanced", L"light" }))
        translator_->setPreset(wideToUtf8(preset));
    // The pair. One half may arrive alone (the contract says only-what-changed travels), so the
    // missing half is filled from what the app currently believes and the two are pushed
    // TOGETHER -- the engine takes a pair or nothing.
    std::wstring a, b;
    const bool okA = trLangCode(langA, a), okB = trLangCode(langB, b);
    if (okA || okB) {
        const std::wstring na = okA ? a : trLangA_;
        const std::wstring nb = okB ? b : trLangB_;
        // The same language twice is not a pair: the engine refuses it outright, so accepting it
        // here would leave the app's mirror claiming a pair that never took effect.
        if (na != nb) {
            trLangA_ = na;
            trLangB_ = nb;
            translator_->setPair(wideToUtf8(na), wideToUtf8(nb));
        }
    }
    if (clear) {
        // AudioPB: a Clear ended the LIVE session (its log, its session file). SUVTITLE LIVE (1.1.0): the live session is
        // the Live WINDOW's now, and so is its Clear (handleLiveCtl): the player's page clearing its own screen must not
        // wipe the live path's queue -- it still hears `cleared`, and nothing reaches the engine.
        postToWeb(L"{\"type\":\"translatorState\",\"cleared\":true}");
    }
    sendTranslatorState(/*force=*/true);
}

void Bridge::sendTranslatorState(bool force) {
    const std::wstring slot = translator_ ? utf8ToWide(translator_->slotName().c_str())
                                          : std::wstring();
    // W2: what the user ASKED the engine to be, as opposed to `preset` below, which is what the
    // engine resolved it to. OMITTED while it is empty ("the user has never chosen").
    const std::wstring presetReq = translator_ ? utf8ToWide(translator_->preset().c_str())
                                               : std::wstring();
    // SuVtitle: `active` is always true (the engine is always wanted: AudioPB's was the card's existence, "off"
    // without it). AudioPB's live-only fields went with the live translator: `spk` (the voiceprint stage) and
    // `sessionLines` (the live session file) -- a page that reads them gets its own defaults (off, 0).
    std::wstring json = L"{\"type\":\"translatorState\",\"active\":true"
        + std::wstring(L",\"running\":") + (trRunning_ ? L"true" : L"false")
        + L",\"status\":\"" + jsonEscape(trStatus_) + L"\""
        + L",\"preset\":\"" + jsonEscape(trPreset_) + L"\""
        + (presetReq.empty() ? L"" : L",\"presetReq\":\"" + jsonEscape(presetReq) + L"\"")
        + L",\"mode\":\""   + jsonEscape(trMode_)   + L"\""
        // The PAIR the direction runs over (W2). Always present and never empty: the page dresses its language bar
        // and its "Show A / Show B" labels from these two.
        + L",\"langA\":\"" + jsonEscape(trLangA_) + L"\""
        + L",\"langB\":\"" + jsonEscape(trLangB_) + L"\""
        + L",\"gpu\":" + (trGpu_ ? L"true" : L"false")
        + L",\"slot\":\"" + jsonEscape(slot) + L"\""
        + L",\"models\":{\"asr\":\"" + jsonEscape(trAsrModel_)
        + L"\",\"mt\":\"" + jsonEscape(trMtModel_) + L"\"}"
        // SUVTITLE LIVE (1.1.0): the live path, for the player's notice ("Live translation is on -- this film's subtitles
        // will be made when it stops", LIVE_PLAN 3.3): live.on, and the rest of the object both states carry.
        + L",\"live\":" + liveJson()
        + (trErr_.empty() ? L"" : L",\"err\":\"" + jsonEscape(trErr_) + L"\"")
        + L"}";
    sendLiveState(force);   // SUVTITLE LIVE: the Live view's own state (the LIVE pair), diffed on its own
    if (!force && json == lastTranslatorStateJson_) return;
    lastTranslatorStateJson_ = json;
    postToWeb(json);
}

// THE DRAIN'S DOOR (W1). Called by the window proc on kWmTranslatorPoll -- i.e. within a
// message of the helper having something to say -- and by uiTick, which stays as the safety
// net: if a post is ever lost (a full message queue, a window being rebuilt), the cost must
// be one second of lateness, never a subtitle that never arrives.
//
// The flag is cleared BEFORE the drain, not after: a message the worker queues WHILE we are
// draining must be able to buy itself a fresh post. Clearing afterwards would swallow that
// post and leave the message sitting until the timer -- the very bug this replaces, made
// rarer and therefore harder to see.
void Bridge::pumpTranslator() {
    // RV2-6 (2026-10-23): uiTick's guard. A worker's poll still queued when the window closed is dispatched AFTER
    // WM_DESTROY's shutdown (before WM_QUIT): it must not feed a scan / probe / extraction to a player that was shut
    // down (cache and .srt writes, a FILE for an engine that is gone).
    if (!running_.load()) return;
    trPollPending_.store(false, std::memory_order_release);
    drainPlayerScans();     // WE: a folder scan's result rides the same wake
    drainPlayerTracks();    // WH: and a probe's / an embedded subtitle's
    drainUpdate();          // THE UPDATE BANNER: and the update check's answer
    pollTranslatorOnce();
    syncNative();           // 1.1.0 NATIVE: a scan that opened a file -> mpv plays it
}

// Warnings, then STATE, then the file messages, in the order the engine sent them.
// SUVTITLE LIVE (1.1.0): ...and the live path's -- LINE / PART / END to the Live view (AudioPB 1.1.0's loop), the picker's
// lists, the inputs' states, then the newest meters; the player's gate follows the wish and the echo.
void Bridge::pollTranslatorOnce() {
    if (!translator_) return;

    // The supervisor's own warnings ("keeps failing to start", "stopped responding; restarting it"). AudioPB showed
    // them on the DESK ({type:"warn"}); SuVtitle has no desk, and its page shows translatorFileNotice -- so there.
    // SUVTITLE LIVE: and the Live window, as 1.1.0's warn{text} -- BOTH views (LIVE_PLAN 3.3): a meeting must hear it.
    for (auto& w : translator_->pollWarnings()) { postTranslatorFileNotice(w); postLiveWarn(w); }

    pbr::TranslatorManager::State st;
    bool dirty = false;
    if (translator_->pollState(st)) {
        trStatus_    = utf8ToWide(st.status.c_str());
        trAsrModel_  = utf8ToWide(st.asrModel.c_str());
        trMtModel_   = utf8ToWide(st.mtModel.c_str());
        trErr_       = utf8ToWide(st.err.c_str());
        trGpu_       = st.gpu;
        // The engine OWNS its config, so what it reports is the truth about mode/preset/pair -- the app mirrors it
        // rather than asserting its own idea. A code the engine could not honour never appears in this echo (TrPipe.h).
        if (!st.preset.empty()) trPreset_ = utf8ToWide(st.preset.c_str());
        if (!st.mode.empty())   trMode_   = utf8ToWide(st.mode.c_str());
        if (!st.langA.empty())  trLangA_  = utf8ToWide(st.langA.c_str());
        if (!st.langB.empty())  trLangB_  = utf8ToWide(st.langB.c_str());
        // SUVTITLE LIVE (1.1.0): the live half of the echo -- whether the live path runs, ITS pair (empty = not reported:
        // an engine without the live mode; the mirror keeps what it has, TrPipe.h), and the voiceprint stage (1.1.0).
        if (!st.liveMode.empty())  liveMode_  = utf8ToWide(st.liveMode.c_str());
        if (!st.liveLangA.empty()) liveLangA_ = utf8ToWide(st.liveLangA.c_str());
        if (!st.liveLangB.empty()) liveLangB_ = utf8ToWide(st.liveLangB.c_str());
        trSpkOn_    = st.spkOn;
        trSpkModel_ = utf8ToWide(st.spkModel.c_str());
        trSpkCount_ = st.spkCount;
        if (st.live != trLive_) {
            trLive_ = st.live;
            if (!trLive_) for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }   // not listening: no lamp lit
        }
        dirty = true;
        // WE: the pair and direction RUNNING now are what a new file job (and the badges' cache
        // keys) are made with -- the echo, never the app's optimistic copy.
        if (player_) player_->setPair(wideToUtf8(trLangA_), wideToUtf8(trLangB_), wideToUtf8(trMode_));
    }
    // SUVTITLE LIVE (RV2-5 on the live path): the manager DROPPED the wanted LIVE -- the engine kept dying right after
    // being asked to listen (its warning says so): the app's wish follows, the Listen button goes off.
    if (translator_->pollLiveDropped()) { liveWanted_ = false; dirty = true; }
    // SUVTITLE LIVE fix round (RVB-5): the engine REFUSED the LIVE (a test input outside a test engine, a capture that
    // would not start): the manager dropped the wish -- the app's follows (never "starting to listen..." for ever, the
    // player's gate opens again), and the Live window says why.
    {
        std::string why;
        if (translator_->pollLiveRefused(why)) {
            liveWanted_ = false;
            for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }
            postLiveWarn(utf8ToWide(why.c_str()));
            dirty = true;
        }
    }
    updateLiveGate();   // SUVTITLE LIVE: the player's gate = the wish || the echo
    const bool run = translator_->running();
    if (run != trRunning_) { trRunning_ = run; dirty = true; }
    if (dirty) sendTranslatorState(/*force=*/false);

    // SUVTITLE LIVE (1.1.0 W5): the session's line count before this drain, so ONE state message covers a burst of lines
    // (the tooltip is a tooltip, not a meter) -- and one for the inputs' lamps.
    const long long linesBefore = trSessionLines_;
    bool liveDirty = false;
    using MsgKind = pbr::TranslatorManager::Msg::Kind;
    for (auto& msg : translator_->pollMessages()) {
        // ---- FILE MODE (WC/WE): a recording's progress, cues and end, to the player --------
        // Same ordered queue as everything else; each cue goes to the window as it arrives, the
        // session state once at the end of the drain (player_->flush, after every cue it closes).
        // A synthesized FDONE (the engine died) is said to be one: it vouches for nothing.
        if (msg.kind == MsgKind::FileProg || msg.kind == MsgKind::FileCue
            || msg.kind == MsgKind::FileDone || msg.kind == MsgKind::ProxyDone || msg.kind == MsgKind::FileSpk) {
            if (!player_) continue;
            if (msg.kind == MsgKind::FileProg)      player_->onProg(msg.fprog);
            else if (msg.kind == MsgKind::FileCue)  player_->onCue(msg.cue);
            else if (msg.kind == MsgKind::FileSpk)  player_->onSpk(msg.spk);          // U3: the voice pass' names
            else if (msg.kind == MsgKind::FileDone) player_->onDone(msg.fdone, msg.synth);
            else                                    player_->onProxyDone(msg.pdone);
            continue;
        }
        // =====================================================================================================
        // SUVTITLE LIVE (1.1.0): the live path. LINE / PART / END are AudioPB 1.1.0's (50f79d6, pollTranslatorOnce),
        // sent to the LIVE VIEW ONLY (LIVE_PLAN 3.3: the player's page has no live line); the speaker is a STRIP.
        // =====================================================================================================
        // ---- the picker's list (whole: the manager assembled it) -----------------------------------------------
        if (msg.kind == MsgKind::Sources) {
            // OUR gen goes to the page: the engine's restarts with each incarnation, and a page that kept the newest
            // gen would drop the first list of a respawned engine. The keys are remembered: a strip the page adds must
            // name one of them (handleLiveConfig) -- no string of the page's own ever reaches the engine.
            liveSrcKeys_.clear();
            liveSrcLabels_.clear();
            std::wstring list = L"[";
            for (const auto& s : msg.srcs) {
                if (s.kind != "in" && s.kind != "out" && s.kind != "app") continue;   // (wav / ain are never listed)
                liveSrcKeys_.insert(s.key);
                liveSrcLabels_[s.key] = s.label;
                JsonW j;
                j.obj().kv(L"kind", utf8ToWide(s.kind.c_str())).kv(L"key", utf8ToWide(s.key.c_str()))
                       .kv(L"label", utf8ToWide(s.label.c_str())).kv(L"state", utf8ToWide(s.state.c_str()))
                       .kv(L"rate", static_cast<long long>(s.rate)).kv(L"ch", static_cast<long long>(s.ch))
                       .kv(L"flags", utf8ToWide(s.flags.c_str())).end();
                if (list.size() > 1) list += L",";
                list += j.take();
            }
            liveSrcList_ = list + L"]";
            ++liveSrcGen_;
            sendLiveSources();
            continue;
        }
        // ---- an input's state CHANGED (open | absent | busy | denied | unsupported | stalled | error), and why --------
        // INPUT is keyed by the DEVICE (TrPipe.h LANES): its state is the state of EVERY strip on it -- two strips on two
        // channels of one device light together, as they fail together.
        if (msg.kind == MsgKind::Input) {
            for (auto& s : liveStrips_)
                if (s.key == msg.input.key) { s.state = msg.input.state; s.why = msg.input.why; liveDirty = true; }
            continue;
        }
        // ---- the END of an utterance (W3) --------------------------------------
        // "That one produced nothing more": the tail after the last committed sentence was
        // a breath, so no closing LINE will ever arrive and the live line has to fall NOW --
        // without this it hangs until the UI's 20 s orphan timer, i.e. long into the next
        // speaker. Forwarded to NO scrollback, and that is not an oversight: a replay is a
        // transcript, an END is not a record of anything that was said, and a surface that
        // reloads has no live line to clear in the first place.
        if (msg.kind == MsgKind::End) {
            postToLive(L"{\"type\":\"translatorEnd\",\"segId\":"
                       + std::to_wstring((unsigned long long)msg.end.segId) + L"}");
            continue;
        }
        // ---- the LIVE LINE ------------------------------------------------------
        // Forwarded to NO scrollback: a draft is not a record of what was said, it is a
        // placeholder for a record that has not arrived. Putting it in translatorLog_ would
        // replay half-sentences into a window opened an hour later, and there would be no
        // final to replace them with -- the final that superseded it never went in twice.
        //
        // No speaker either, and that is the contract rather than an omission (TrPipe.h):
        // attribution happens once, on the final. A live line that changed colour
        // mid-sentence would be a bug wearing a feature's clothes.
        if (msg.kind == MsgKind::Partial) {
            const auto& p = msg.part;
            std::wstring pj = L"{\"type\":\"translatorPartial\",\"segId\":"
                + std::to_wstring((unsigned long long)p.segId)
                + L",\"t\":"      + std::to_wstring((long long)p.tEpochMs)
                + L",\"lang\":\"" + jsonEscape(utf8ToWide(p.lang.c_str())) + L"\""
                + L",\"src\":\""  + jsonEscape(utf8ToWide(p.src.c_str()))  + L"\"";
            // dst is OMITTED, not empty-stringed, when there is no draft translation
            // yet: "I have nothing to show you" and "the translation is the empty
            // string" are different claims, and only one of them is true here.
            if (!p.dst.empty())
                pj += L",\"dst\":\"" + jsonEscape(utf8ToWide(p.dst.c_str())) + L"\"";
            // ...and dstLang is omitted on exactly the same terms, because it is a property OF
            // that draft: an empty dst has no language, and inventing one would make a window
            // filtered on "show me zh only" display an empty line (TrPipe.h says the same).
            if (!p.dstLang.empty())
                pj += L",\"dstLang\":\"" + jsonEscape(utf8ToWide(p.dstLang.c_str())) + L"\"";
            pj += L"}";
            postToLive(pj);
            continue;
        }
        if (msg.kind != MsgKind::Line) continue;

        const auto& l = msg.line;
        const std::wstring slot  = utf8ToWide(l.spkSlot.c_str());    // the LANE ID of the dominant strip (TrPipe.h LANES)
        const std::wstring sLang = utf8ToWide(l.spkLang.c_str());
        const std::wstring clus  = utf8ToWide(l.spkLabel.c_str());   // "S1".."S8", or ""

        // THE KEY IS WHAT COLOURS ARE HELD BY, so it is built from the strongest
        // identity available for this line, and never from anything the engine had to
        // guess twice.
        //   * with a voiceprint: {strip, cluster}. The strip STAYS in the key on
        //     purpose -- one model deciding that the man on the SM7B and the man on the
        //     conference phone are "the same voice" would merge two people who are
        //     provably in different places, and no amount of cosine says otherwise.
        //   * without one: {strip, language}, T1's rule, unchanged.
        // The strip is named by its LANE ID (TrPipe.h LANES): "canal 1 = moi, canal 2 = l'invite" on one interface are
        // two strips -- two labels, two colours -- never one speaker found by the device's key.
        // THE LABEL (LIVE_PLAN 4.3, a change from 1.1.0, which showed a bare "S2" when one card was patched): every strip
        // has a name the user chose -- the speaker's -- so the label IS the strip's label. Once a strip has produced lines
        // from two voices or more, its NEW lines carry " . <n>" (n = the voice's order of first appearance WITHIN that
        // strip); lines already shown keep theirs (their colours already differ). Renaming a strip renames the lines that
        // FOLLOW, never a colour: the key does not hold the label. (The separator is a \u00B7 ESCAPE, not a typed
        // character: the build does not pass /utf-8.)
        // " . voice <n>", not " . <n>" (26/10, the user's first live session): a strip is very often named after its
        // DEVICE -- "Input 1/2 (3- Steinberg UR44C)" -- and "Input 1/2 (...) . 2" read as INPUT 2, on a mono mic patched
        // to input 1. The word says what the number counts.
        // FIX ROUND (RVB-6, the ADDENDUM's speaker EPOCH): every engine names its clusters from S1 again, so a cluster
        // name alone is no one: the VOICE is {cluster, this run, the engine incarnation that heard it} -- after a respawn
        // (or a restart that reattached the session) the new S1 is a new voice: a new colour, the next number, never the
        // colour or the " . n" of the person who was S1 before. (A Clear starts the session over: endLiveSession.)
        std::wstring key, label = stripLabelFor(slot);
        if (!clus.empty()) {
            const std::wstring voice = clus + L"~" + spkRun_ + L"." + std::to_wstring(msg.gen);
            key = slot.empty() ? voice : (slot + L"#" + voice);
            std::vector<std::wstring>& voices = spkVoices_[slot];
            size_t n = 0;
            while (n < voices.size() && voices[n] != voice) ++n;
            if (n == voices.size()) voices.push_back(voice);
            if (voices.size() >= 2) label = label.empty() ? clus : (label + L" \u00B7 voice " + std::to_wstring(n + 1));
            else if (label.empty()) label = clus;
        } else {
            key = slot.empty() ? std::wstring() : (slot + L"|" + sLang);
        }
        const int color = key.empty() ? 0 : translatorSpeakerColor(key);

        const std::wstring json = L"{\"type\":\"translatorLine\",\"t\":"
            + std::to_wstring((long long)l.tEpochMs)
            + L",\"lang\":\"" + jsonEscape(utf8ToWide(l.lang.c_str())) + L"\""
            + L",\"src\":\""  + jsonEscape(utf8ToWide(l.src.c_str()))  + L"\""
            + L",\"dst\":\""  + jsonEscape(utf8ToWide(l.dst.c_str()))  + L"\""
            // W2: WHICH language `dst` is in. Not derivable from `lang` any more -- with a free
            // pair the target is whichever half `lang` is not, and a passthrough repeats the
            // source (dstLang == lang). Always emitted: unpackLine reconstructs it for an older
            // engine, so there is never an honest reason for it to be missing here.
            + L",\"dstLang\":\"" + jsonEscape(utf8ToWide(l.dstLang.c_str())) + L"\""
            + L",\"ms\":"     + std::to_wstring((long long)l.ms)
            // segId pairs this paragraph with the live lines that led to it, so the UI
            // knows which one to replace. waitMs is what the user actually waited --
            // from the end of the sentence to this message -- and is the number to show;
            // `ms` is the inference alone and belongs in a tooltip (TrPipe.h).
            + L",\"segId\":"  + std::to_wstring((unsigned long long)l.segId)
            + L",\"waitMs\":" + std::to_wstring((long long)l.waitMs)
            // W3: an utterance is no longer one line. `seq` is the order within segId, and
            // `closes` is whether THIS line ends the utterance. closes:false means the speaker
            // is still talking -- the paragraph gains the sentence and the LIVE LINE STAYS.
            // The REPLAY re-sends this string verbatim, closes:false and all, and that costs
            // nothing: an hour later there is no live line for it to keep standing.
            + L",\"seq\":"    + std::to_wstring((unsigned long)l.seq)
            + L",\"closes\":" + (l.closes ? L"true" : L"false")
            + L",\"pass\":"   + (l.pass ? L"true" : L"false")
            + L",\"spk\":{\"key\":\""  + jsonEscape(key)
            + L"\",\"slot\":\""        + jsonEscape(slot)
            + L"\",\"label\":\""       + jsonEscape(label)
            + L"\",\"color\":"         + std::to_wstring(color) + L"}}";

        while (translatorLog_.size() >= kTranslatorLogMax) translatorLog_.pop_front();
        translatorLog_.push_back(json);
        // W5: the same string, verbatim, into the session file. VERBATIM matters -- the export
        // parses it back with the very helpers that wrote it, so there is exactly one format to
        // keep in step, and a field added to the wire tomorrow is in the transcript for free.
        // Only FINISHED lines: a draft is a placeholder and an END is not a record of anything
        // that was said, which is the same rule translatorLog_ obeys two lines above.
        appendTranslatorSession(json);
        postToLive(json);
    }
    if (player_) player_->flush();   // one translatorFile per drain, however many FPROGs
    // SUVTITLE LIVE: the session count moved (the export's tooltip) or a lamp changed: ONE state for the whole drain.
    // Forced never -- sendTranslatorState diffs, and the one field that changed is enough to make it through.
    if (trSessionLines_ != linesBefore || liveDirty) sendTranslatorState(/*force=*/false);
    // SUVTITLE LIVE: the meters -- the NEWEST levels only (a latest-wins slot, never a queue), ~5 Hz while listening.
    // Keyed by LANE ID as the engine names them (TrPipe.h LANES): the page finds each strip's by its `lane`.
    pbr::TranslatorManager::Levels lv;
    if (translator_->pollLevels(lv) && liveView_ && !lv.peaks.empty()) {
        std::wstring j = L"{\"type\":\"liveLevels\",\"levels\":{";
        bool first = true;
        for (const auto& p : lv.peaks) {
            wchar_t b[32];
            ::swprintf(b, 32, L"%.1f", static_cast<double>(p.second));   // (clamped to [-120, +24] by unpackLevels)
            j += (first ? L"\"" : L",\"") + jsonEscape(utf8ToWide(p.first.c_str())) + L"\":" + b;
            first = false;
        }
        postToLive(j + L"}}");
    }
}

// ============================================================================
// FILE MODE (wave WC, 2026-10-10) -- subtitle a RECORDING.
// + THE PLAYER (wave WE, 2026-10-11) -- a real player around it.
//
// The user's request (WC): a player inside AudioPB that loads an audio or video file, analyses it
// to make a translation BETTER than the live one, and shows it in the translator window as
// subtitles while the analysis runs; for a video, the translation becomes subtitles on the
// picture -- and an .srt next to the file at the end, so VLC and friends pick it up by themselves.
// And then (WE, 11/10): watch after a SHORT load while the translation stays AHEAD of the picture
// and completes in the background; translation on/off (a plain player too); a queue, folders,
// resume, repeat/shuffle, subtitle files, a subtitle style.
//
// The split of work: the HELPER analyses (TrPipe.h, FILE MODE v1 + v2); the PAGE plays and draws;
// ui/TrPlayer.h is the STATE MACHINE in between (queue, session, jobs, ranges, partial cache,
// pre-analysis, mappings) -- and this section is its HANDS (PlayerHost: the pipe, the disk, the
// window's mappings and reloads, the posts) plus what needs Windows UI or a thread: the dialogs,
// the drops' File objects, the folder scans, subtitle files, the export.
//
// SECURITY. A media folder is served to the translator view through a virtual-host folder mapping,
// and the probe proved that mapping is NOT a read barrier (a no-cors fetch reads any file in the
// folder). So: the translator view only; the folders of the files the USER queued (the current one
// and the next <= 15, pbplayer::kMapMax), one host per folder named from it, never re-pointed (a
// re-pointed host keeps serving its old folder to a loaded page -- measured, STEP0 M1); and the page
// that runs there never builds HTML from data (test_ui.js enforces it), never navigates elsewhere
// (main.cpp). Paths never come from a page: dialogs, File objects, or ids minted here.
// ============================================================================

namespace {

const wchar_t* const kTrProxyHost = pbplayer::kProxyHost;
constexpr size_t     kTrCacheKeep  = 500;    // cache records kept (newest); partial ones count too
constexpr size_t     kTrSubChunk   = 1000;   // external subtitle cues per message

// TrPipe.h since WB's study: auto == fast == large-v3-turbo, max == large-v3. The window offers
// "Standard" (auto) and "large-v3" (max); fast is folded into auto so the same result is never
// cached twice under two names.
std::wstring trQuality(const std::wstring& q) { return q == L"max" ? L"max" : L"auto"; }

std::wstring trProxyDir() {
    const std::wstring c = pbtrf::cacheDir();
    return c.empty() ? c : pbtrf::joinPath(c, L"proxy");
}
std::wstring trCachePath(const std::string& key) {
    const std::wstring c = pbtrf::cacheDir();
    return c.empty() ? c : pbtrf::joinPath(c, pbtrf::fromUtf8(key) + L".cues");
}
// U3: the transcript's voices record, beside its analysis' record (the same key: TrFileStore.h, the voices record)
std::wstring trVoicesPath(const std::string& key) {
    const std::wstring c = pbtrf::cacheDir();
    return c.empty() ? c : pbtrf::joinPath(c, pbtrf::fromUtf8(key) + L".voices");
}

// The registry of subtitle files we wrote, and for which media (TrFileStore.h).
std::wstring trRegistryPath() {
    const std::wstring c = pbtrf::cacheDir();
    return c.empty() ? c : pbtrf::joinPath(c, L"srt-written.txt");
}
std::vector<pbtrf::RegEntry> trLoadRegistry() {
    std::string t;
    const std::wstring p = trRegistryPath();
    if (!p.empty()) pbtrf::readFileAll(p, t);
    return pbtrf::parseRegistry(t);
}
// WH (D6): an automatic per-language file also records its language and the record whose cues it holds (+ that
// record's sound track); an export passes none.
void trRegister(const std::wstring& srt, uint64_t size, uint64_t mtime, const std::wstring& media,
                const std::string& lang = std::string(), const std::string& key = std::string(), uint32_t ord = 0) {
    const std::wstring p = trRegistryPath();
    if (p.empty()) return;
    std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    pbtrf::registryPut(reg, srt, size, mtime, media, lang, key, ord);
    pbtrf::writeFileAvSafe(p, pbtrf::packRegistry(reg));
}
// The leaves in `dir` that share `leaf`'s base ("Interview_01.*") -- for hasSiblingMedia.
std::vector<std::wstring> trLeavesWithBase(const std::wstring& dir, const std::wstring& leaf) {
    std::vector<std::wstring> out;
    const std::wstring base = pbtrf::stripExt(leaf);
    if (dir.empty() || base.empty()) return out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pbtrf::joinPath(dir, base + L".*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (::FindNextFileW(h, &fd) && out.size() < 512);
    ::FindClose(h);
    return out;
}

std::wstring trDocumentsSubtitlesDir() {
    PWSTR docs = nullptr;
    std::wstring d;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        d = std::wstring(docs) + L"\\SuVtitle Subtitles";
        ::CoTaskMemFree(docs);
    }
    return d;
}

std::wstring trDocumentsDir() {
    PWSTR docs = nullptr;
    std::wstring d;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
        d = docs;
        ::CoTaskMemFree(docs);
    }
    return d;
}
// U3: where a transcript goes when its media's folder refuses the write (never in a test instance: the user's real Documents)
std::wstring trDocumentsTranscriptsDir() {
    const std::wstring d = trDocumentsDir();
    return d.empty() ? d : d + L"\\SuVtitle Transcripts";
}

std::string  u8(const std::wstring& w) { return pbtrf::toUtf8(w); }
std::wstring w16(const std::string& s) { return pbtrf::fromUtf8(s); }

// U3: the transcript's Word document, as bytes (DocxWriter.cpp buildSpeakerTranscript): its header text from TrTranscript.h
// docTextOf (the date = this PC's local time; the languages = DocxWriter's native names, + the English name after a non-Latin
// script), one paragraph per turn. False = no turn (never an empty Word file).
bool trTranscriptBytes(const pbtrs::TranscriptOut& t, std::string& bytes) {
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char date[40];
    std::snprintf(date, sizeof date, "%04u-%02u-%02u %02u:%02u", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                  (unsigned)st.wHour, (unsigned)st.wMinute);
    const pbtrs::DocText d = pbtrs::docTextOf(t, date, SUV_VERSION_STR,
                                              [](const std::string& c) { return pbtrf::toUtf8(pbdocx::languageName(pbtrf::fromUtf8(c))); });
    pbdocx::TranscriptDoc doc;
    doc.title = w16(d.title);
    doc.subtitle = w16(d.subtitle);
    doc.note = w16(d.note);
    for (const auto& h : d.header) doc.header.push_back(w16(h));
    std::vector<pbdocx::TranscriptTurn> turns;
    turns.reserve(t.turns.size());
    for (const auto& x : t.turns) {
        pbdocx::TranscriptTurn tt;
        tt.stamp = w16(x.stamp); tt.speaker = w16(x.name); tt.text = w16(x.text); tt.lang = w16(x.lang);
        turns.push_back(std::move(tt));
    }
    bytes.clear();
    return pbdocx::buildSpeakerTranscript(doc, turns, bytes) && !bytes.empty();
}

// "1:42:10" / "12:31" -- for the transcript's header line.
std::wstring trClock(uint64_t ms) {
    const unsigned long long s = ms / 1000ULL;
    wchar_t b[32];
    if (s >= 3600) std::swprintf(b, 32, L"%llu:%02llu:%02llu", s / 3600, (s / 60) % 60, s % 60);
    else           std::swprintf(b, 32, L"%llu:%02llu", s / 60, s % 60);
    return b;
}

// WG: the cues of ONE language, as the SRT writer takes them (its text in `dst`, Which::Dst): each cue's text IN
// that language -- TrFileStore.h textInLang -- in START order; a cue with none is not in the file.
std::vector<pbsub::Cue> trLangCues(std::vector<pbt::trpipe::WireCue> cues, const std::string& lang) {
    pbtrf::canonicalCues(cues);
    std::vector<pbsub::Cue> out;
    out.reserve(cues.size());
    std::string t;
    for (const auto& c : cues) {
        if (!pbtrf::textInLang(c, lang, t)) continue;
        pbsub::Cue s;
        s.startMs = c.startMs; s.endMs = c.endMs; s.lang = lang; s.dstLang = lang;
        s.dst = t;
        out.push_back(std::move(s));
    }
    return out;
}

// WG editor: a subtitle FILE's runs as the SRT markup the editor shows and a file is written with.
std::string trRunsToTagged(const std::vector<pbsubr::Run>& runs) {
    std::string o;
    for (const auto& r : runs) {
        std::string t = r.text;
        if (r.flags & pbsubr::kUnderline) t = "<u>" + t + "</u>";
        if (r.flags & pbsubr::kItalic) t = "<i>" + t + "</i>";
        if (r.flags & pbsubr::kBold) t = "<b>" + t + "</b>";
        o += t;
    }
    return o;
}
// WebVTT text is markup: & < > escaped -- except the three tags the editor speaks (<i> <b> <u> and their ends).
std::string trVttLine(const std::string& l) {
    std::string o = pbsub::escVtt(l);
    for (const char* t : { "i", "b", "u" }) {
        const std::string open = std::string("&lt;") + t + "&gt;", close = std::string("&lt;/") + t + "&gt;";
        for (size_t at = 0; (at = o.find(open, at)) != std::string::npos; ) { o.replace(at, open.size(), std::string("<") + t + ">"); at += 3; }
        for (size_t at = 0; (at = o.find(close, at)) != std::string::npos; ) { o.replace(at, close.size(), std::string("</") + t + ">"); at += 4; }
    }
    return o;
}
// An edited subtitle file's bytes: SubRip (or WebVTT), UTF-8 with a BOM, CRLF; each cue's lines AS TYPED (the markup
// kept, its line breaks kept, no blank line inside -- a blank line ENDS a cue; "-->" defused in SRT).
std::string trExtBytes(const std::vector<TrExtCue>& cues, bool vtt) {
    std::string out = vtt ? std::string("\xEF\xBB\xBF" "WEBVTT\r\n\r\n") : std::string("\xEF\xBB\xBF");
    unsigned n = 0;
    for (const auto& c : cues) {
        std::vector<std::string> lines;
        size_t at = 0;
        while (at <= c.text.size()) {
            const size_t nl = c.text.find('\n', at);
            std::string l = c.text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
            while (!l.empty() && (l.back() == ' ' || l.back() == '\r')) l.pop_back();
            if (!l.empty()) lines.push_back(l);
            if (nl == std::string::npos) break;
            at = nl + 1;
        }
        if (lines.empty()) continue;                       // a cue with no text would END itself at once
        out += std::to_string(++n) + "\r\n";
        out += pbsub::stamp(c.s, vtt ? '.' : ',') + " --> " + pbsub::stamp(c.e > c.s ? c.e : c.s + 1, vtt ? '.' : ',') + "\r\n";
        for (const auto& l : lines) { out += vtt ? trVttLine(l) : pbsub::srtSafe(l); out += "\r\n"; }
        out += "\r\n";
    }
    return out;
}

// The writer's cues, in START order: v2's runs arrive interleaved in time (TrPipe.h).
std::vector<pbsub::Cue> trSubCues(std::vector<pbt::trpipe::WireCue> cues) {
    pbtrf::canonicalCues(cues);
    std::vector<pbsub::Cue> out;
    out.reserve(cues.size());
    for (const auto& c : cues) {
        pbsub::Cue s;
        s.startMs = c.startMs; s.endMs = c.endMs; s.lang = c.lang; s.dstLang = c.dstLang;
        s.pass = c.pass; s.src = c.src; s.dst = c.dst;
        out.push_back(std::move(s));
    }
    return out;
}

}   // namespace

// ---- the core's hands (pbplayer::Host) -------------------------------------------------------------
void Bridge::PlayerHost::sendFile(const pbt::trpipe::WireFile& f) { if (b.translator_) b.translator_->startFile(f); }
void Bridge::PlayerHost::sendFocus(uint64_t wire, uint64_t ms) { if (b.translator_) b.translator_->focusFile(wire, ms); }
void Bridge::PlayerHost::sendCancel(uint64_t id) { if (b.translator_) b.translator_->cancelFile(id); }
void Bridge::PlayerHost::sendProxy(const pbt::trpipe::WireProxy& p) { if (b.translator_) b.translator_->requestProxy(p); }
// 1.1.0 NATIVE (fix round, L2): mpv plays the current item -- or is about to (its opening follows the core's load at once):
// the core then never asks for the sound copy (TrPlayer.h mapNow / playFailed): it exists for the page's own <video> only
bool Bridge::PlayerHost::mpvPlays() { return b.nativeWanted(); }
void Bridge::PlayerHost::postBoth(const std::wstring& json) { b.postToWeb(json); }   // UI thread: THE view (one window)
void Bridge::PlayerHost::postWindow(const std::wstring& json) { b.postToTranslatorView(json); }
// The MAIN view only: the core's state while the window's page reloads (TrPlayer.h windowBusy, review
// of WE P1 -- a post then would reach the OLD document). UI thread, like every core call.
// SuVtitle: there is no main view (AudioPB's desk showed the file's progress on the Translator card). Nothing to do:
// the reloading page gets the whole state on its `ready` (TrPlayer.h windowReady).
void Bridge::PlayerHost::postMain(const std::wstring& json) { (void)json; }
void Bridge::PlayerHost::openWindow() {
    const HWND h = b.hostWnd_.load();
    if (h) ::PostMessageW(h, kWmTranslatorWindow, 0, 0);
}
void Bridge::PlayerHost::mapFolders(const std::vector<std::wstring>& folders, std::vector<std::wstring>& failed) {
    b.mapPlayerFolders(folders, failed);
}
// MEASURED (WC review; STEP0 M1): a page sees only the mappings that existed when it navigated. The
// session comes back whole on its `ready` (TrPlayer.h windowReady).
void Bridge::PlayerHost::reloadWindow() {
    if (!b.webview_) return;
    b.trWin_.reloading();                    // nothing more to it until its `ready` (C1, P1)
    b.webview_->Reload();
}
bool Bridge::PlayerHost::identity(const std::wstring& path, uint64_t& size, uint64_t& mtime) {
    bool isDir = false;
    return pbtrf::fileIdentity(path, size, mtime, &isDir) && !isDir;
}
bool Bridge::PlayerHost::readCache(const std::string& key, pbtrf::Record& r) {
    std::string text;
    const std::wstring p = trCachePath(key);
    return !p.empty() && pbtrf::readFileAll(p, text) && pbtrf::parseRecord(text, r);
}
bool Bridge::PlayerHost::peekCache(const std::string& key, pbtrf::Summary& s) {
    const std::wstring p = trCachePath(key);
    return !p.empty() && pbtrf::peekRecord(p, s);
}
void Bridge::PlayerHost::writeCache(const std::string& key, const pbtrf::Record& r) {
    const std::wstring p = trCachePath(key);
    if (p.empty()) return;
    pbtrf::writeFileAvSafe(p, pbtrf::packRecord(r));
    pbtrf::keepNewest(pbtrf::cacheDir(), L"*.cues", kTrCacheKeep);
}
std::wstring Bridge::PlayerHost::writeSrt(const pbplayer::CueSet& set, const std::string& lang,
                                          const std::vector<pbt::trpipe::WireCue>& cues,
                                          const std::string& fromKey, uint32_t fromOrd,
                                          std::wstring& note, uint64_t& size, uint64_t& mtime) {
    return b.writeTranslatorSrt(set, lang, cues, fromKey, fromOrd, note, size, mtime);
}
// WH (D6): the record whose cues the automatic file of `lang` holds now -- the registry's newest writer for this media and
// language, if that file is still exactly what was written (one stat; the registry is read like writeTranslatorSrt does).
std::string Bridge::PlayerHost::srtWriter(const pbplayer::CueSet& set, const std::string& lang) {
    const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    const pbtrf::RegEntry* e = pbtrf::registryWriter(reg, set.path, lang);
    if (!e) return std::string();
    uint64_t s = 0, m = 0;
    if (!pbtrf::fileIdentity(w16(e->path), s, m) || s != e->size || m != e->mtime) return std::string();
    return e->key;
}
void Bridge::PlayerHost::saveAudio(const std::string& bytes) {
    const std::wstring p = b.appDataPath(L"player-audio.txt");
    if (!p.empty()) pbtrf::writeFileAvSafe(p, bytes);
}
// WH: a probe on its own thread (headers only: the head of a Matroska file, the small boxes of an MP4's moov); the answer
// goes through the TrackBox and the translator's wake. The flag is the tray's end only: a probe is too short to cancel.
void Bridge::PlayerHost::probeTracks(uint64_t req, const std::wstring& path) {
    std::shared_ptr<TrackBox> box = b.trackBox_;
    if (!box) return;
    const HWND h = b.hostWnd_.load();
    std::thread([box, req, path, h] {
        auto info = std::make_shared<pbmedia::Info>(pbmedia::probeFile(path, &box->quit));
        {
            std::lock_guard<std::mutex> lk(box->mx);
            box->probes.push_back(TrackBox::Probe{ req, info });
        }
        if (h && !box->quit.load()) ::PostMessageW(h, kWmTranslatorPoll, 0, 0);
    }).detach();
}
// WH (D7): one embedded text subtitle track -- extracted (the Cues, else a header walk), synthesized and READ by
// SubtitleReader on its own thread; cancellable (a new request, another item, a file loaded, the tray's end).
void Bridge::PlayerHost::extractSub(uint64_t req, const std::wstring& path, std::shared_ptr<const pbmedia::Info> info, uint32_t track) {
    std::shared_ptr<TrackBox> box = b.trackBox_;
    if (!box || !info) return;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(box->mx);
        box->cancels[req] = cancel;
    }
    const HWND h = b.hostWnd_.load();
    std::thread([box, req, path, info, track, cancel, h] {
        auto res = std::make_shared<pbmedia::SubResult>();
        pbmedia::extractFile(path, *info, track, *res, cancel.get());
        {
            std::lock_guard<std::mutex> lk(box->mx);
            box->cancels.erase(req);
            box->subs.push_back(TrackBox::Sub{ req, res });
        }
        if (h && !box->quit.load()) ::PostMessageW(h, kWmTranslatorPoll, 0, 0);
    }).detach();
}
void Bridge::PlayerHost::cancelExtract(uint64_t req) {
    std::shared_ptr<TrackBox> box = b.trackBox_;
    if (!box) return;
    std::lock_guard<std::mutex> lk(box->mx);
    auto it = box->cancels.find(req);
    if (it != box->cancels.end()) it->second->store(true);
}
std::wstring Bridge::PlayerHost::proxyPath(const std::string& mediaKey, uint64_t id) {
    const std::wstring pd = trProxyDir();
    if (pd.empty() || !pbtrf::ensureDir(pd)) return std::wstring();
    if (!b.trProxyPurged_) {   // proxies an earlier run left behind (in use when it closed)
        b.trProxyPurged_ = true;
        pbtrf::keepNewest(pd, L"*.wav", 0);
        // ...and the half-written ones: the helper writes <name>.wav.tmp then renames, and a
        // cancel whose delete lost the race with the AV scan leaves the .tmp (multi-GB) behind.
        pbtrf::keepNewest(pd, L"*.wav.tmp", 0);
    }
    // Named by the MEDIA's identity (plus the request): two files never share a proxy URL, in this
    // run or the next (review of WC).
    return pbtrf::joinPath(pd, w16(mediaKey) + L"-" + std::to_wstring(id) + L".wav");
}
void Bridge::PlayerHost::deleteFile(const std::wstring& path) { if (!path.empty()) ::DeleteFileW(path.c_str()); }   // in use -> purged next run
// The subtitle files that belong to this medium by NAME (SubtitleReader.h isSidecarFor) -- except an
// .srt AudioPB itself wrote FOR THIS MEDIA: its subtitles are the AudioPB source already.
std::vector<pbplayer::Sidecar> Bridge::PlayerHost::sidecars(const std::wstring& folder, const std::wstring& leaf) {
    std::vector<pbplayer::Sidecar> out;
    const std::wstring base = pbtrf::stripExt(leaf), media = pbtrf::joinPath(folder, leaf);
    if (folder.empty() || base.empty()) return out;
    WIN32_FIND_DATAW fd{};
    HANDLE h = ::FindFirstFileW(pbtrf::joinPath(folder, base + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    size_t seen = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring hint;
        if (!pbsubr::isSidecarFor(leaf, fd.cFileName, hint)) continue;
        const std::wstring p = pbtrf::joinPath(folder, fd.cFileName);
        uint64_t s = 0, m = 0;
        if (pbtrf::fileIdentity(p, s, m) && pbtrf::registryOwns(reg, p, s, m, media)) continue;
        pbplayer::Sidecar sc;
        sc.name = fd.cFileName; sc.hint = hint; sc.path = p;
        out.push_back(sc);
    } while (::FindNextFileW(h, &fd) && ++seen < 2000);
    ::FindClose(h);
    std::sort(out.begin(), out.end(), [](const pbplayer::Sidecar& x, const pbplayer::Sidecar& y) {
        return pbplayer::naturalLess(x.name, y.name);
    });
    if (out.size() > 20) out.resize(20);
    return out;
}
void Bridge::PlayerHost::saveQueue(const std::string& bytes) {
    const std::wstring p = b.appDataPath(L"player.txt");
    if (!p.empty()) pbtrf::writeFileAvSafe(p, bytes);
}
void Bridge::PlayerHost::saveResume(const std::string& bytes) {
    const std::wstring p = b.appDataPath(L"player-resume.txt");
    if (!p.empty()) pbtrf::writeFileAvSafe(p, bytes);
}
uint64_t Bridge::PlayerHost::nowMs() { return ::GetTickCount64(); }
uint64_t Bridge::PlayerHost::epochSec() { return static_cast<uint64_t>(std::time(nullptr)); }
// ---- U3 (02/11/2026, the TRANSCRIPTION -- docs/PROGRESS_U3.md) ----
// The voice pass: TranslatorManager::startVoice (the FILE job's slot; exactly one FDONE; FSPK before an FDONE ok).
void Bridge::PlayerHost::sendVoice(const pbt::trpipe::WireVoice& v) { if (b.translator_) b.translator_->startVoice(v); }
// <cache>\<key>.voices -- the analysis' key (TrFileStore.h, the voices record): read whole, peeked (16 KB: the badges),
// written AV-safe; kept like the records (the newest kTrCacheKeep).
bool Bridge::PlayerHost::readVoices(const std::string& key, pbtrf::VoiceRec& v) {
    std::string text;
    const std::wstring p = trVoicesPath(key);
    return !p.empty() && pbtrf::readFileAll(p, text) && pbtrf::parseVoicesText(text, v);
}
bool Bridge::PlayerHost::peekVoices(const std::string& key, pbtrf::VoiceRec& v) {
    const std::wstring p = trVoicesPath(key);
    return !p.empty() && pbtrf::peekVoices(p, v);
}
void Bridge::PlayerHost::writeVoices(const std::string& key, const pbtrf::VoiceRec& v) {
    const std::wstring p = trVoicesPath(key);
    if (p.empty()) return;
    pbtrf::writeFileAvSafe(p, pbtrf::packVoices(v));
    pbtrf::keepNewest(pbtrf::cacheDir(), L"*.voices", kTrCacheKeep);
}
// The source timecode on its own thread (a few header reads, bounded: SourceTimecode.h's budget; a share may be slow); the
// answer goes through the TrackBox and the translator's wake, like a probe's. Any failure is "none" (file-relative stamps).
void Bridge::PlayerHost::probeTimecode(uint64_t req, const std::wstring& path) {
    std::shared_ptr<TrackBox> box = b.trackBox_;
    if (!box) return;
    const HWND h = b.hostWnd_.load();
    std::thread([box, req, path, h] {
        const pbtc::SourceTc tc = box->quit.load() ? pbtc::SourceTc{} : pbtc::readFileTimecode(path);
        {
            std::lock_guard<std::mutex> lk(box->mx);
            box->tcs.push_back(TrackBox::Tc{ req, tc });
        }
        if (h && !box->quit.load()) ::PostMessageW(h, kWmTranslatorPoll, 0, 0);
    }).detach();
}
// FX (F2, the user's rule 02/11: ONE Word file per media AND sound track): which analysis' transcript a Word file holds now --
// `path` given: the registry's transcript entry for THAT file; `path` empty: this set's sound track's Word file (the newest entry
// SuVtitle wrote for this media and track whose file is still there). Its analysis key, "" = none known. (The core asks it only
// for a transcript with no Word file of its own, or one whose file changed since it was written: TrPlayer.h docxState.)
std::string Bridge::PlayerHost::docxWriter(const pbplayer::CueSet& set, const std::wstring& path) {
    const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    if (!path.empty()) {
        const pbtrf::RegEntry* e = pbtrf::registryTranscriptEntry(reg, path);
        return e ? e->key : std::string();
    }
    return pbtrf::registryTrackDocx(reg, set.path, set.audio.ord, [](const std::wstring& p) { return pbtrf::fileExists(p); });
}

// ---- the windows ---------------------------------------------------------------------------------------
void Bridge::postToTranslatorView(const std::wstring& json) {
    // UI thread only -- every file-mode emitter runs there. (SuVtitle: THE view; AudioPB's went to its second
    // window, hidden but alive when closed.)
    if (webview_) webview_->PostWebMessageAsJson(json.c_str());
}

HWND Bridge::dialogOwner() const {
    return trHwnd_ ? trHwnd_ : hostWnd_.load();   // (SuVtitle: one window, every request comes from it)
}

// {type:"translatorWindow", shown, fullscreen[, file]} -- what main.cpp says of the WINDOW, and the
// tray's ONE-SHOT "AudioPB Player..." request (TrWindowState.h; review of WE, C1: it stuck to every
// later post and dragged a Live user back to the File tab). Only to a page that has said `ready`: its
// `ready` gets the whole state anyway.
void Bridge::postTranslatorWindowState() {
    if (webview_ && trWin_.canPost()) postToTranslatorView(trWin_.post());
}

void Bridge::translatorFullscreenChanged(bool on) {
    trWin_.fullscreen = on;
    postTranslatorWindowState();
}

void Bridge::translatorTopmostChanged(bool on) {
    trWin_.topmost = on;
    postTranslatorWindowState();     // (a page still loading gets it on its `ready`)
}

void Bridge::showPlayer() {
    startup();                       // (the queue loads first -- never on top of what is opened)
    trWin_.askFile();
    postTranslatorWindowState();     // a page still loading gets it on its `ready`, once
}

void Bridge::persistPlayer() {
    flushLiveConfig();   // SUVTITLE LIVE fix round (RVB-13): the Windows session ends -- a trim's debounced LIVEIN too
    if (extDoc_ && extDoc_->dirty) { extDoc_->blocked = false; saveExtDoc(); }   // WG: the edited subtitle file too
    if (player_) player_->persist();
}

void Bridge::postTranslatorFileNotice(const std::wstring& text, bool info) {
    JsonW j;
    j.obj().kv(L"type", L"translatorFileNotice").kv(L"text", text);
    if (info) j.kv(L"info", true);
    j.end();
    postToTranslatorView(j.take());
}

// The player's folders, served to the TRANSLATOR view only, one host per folder (named from the
// folder: f<folderKey>.suvtitle-media.example), exactly these and nothing else. A host already mapped
// is left alone (same name = same folder: it is never re-pointed).
void Bridge::mapPlayerFolders(const std::vector<std::wstring>& folders, std::vector<std::wstring>& failed) {
    if (!webview_) { failed = folders; return; }
    ComPtr<ICoreWebView2_3> v3;
    if (FAILED(webview_->QueryInterface(IID_PPV_ARGS(&v3))) || !v3) { failed = folders; return; }
    std::set<std::wstring> want;
    for (const auto& f : folders) want.insert(L"f" + w16(pbtrf::folderKey(f)) + pbplayer::kMediaHostSuffix);
    for (const auto& h : trMappedHosts_)
        if (!want.count(h)) v3->ClearVirtualHostNameToFolderMapping(h.c_str());
    std::set<std::wstring> now;
    for (const auto& f : folders) {
        const std::wstring host = L"f" + w16(pbtrf::folderKey(f)) + pbplayer::kMediaHostSuffix;
        if (trMappedHosts_.count(host)) { now.insert(host); continue; }
        if (SUCCEEDED(v3->SetVirtualHostNameToFolderMapping(host.c_str(), f.c_str(),
                                                            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS)))
            now.insert(host);
        else
            failed.push_back(f);   // an odd UNC share, a vanished drive: the player asks for the sound
    }
    trMappedHosts_ = now;
}

void Bridge::mapTranslatorProxyFolder() {
    if (!webview_) return;
    ComPtr<ICoreWebView2_3> v3;
    const std::wstring pd = trProxyDir();
    if (!pd.empty() && pbtrf::ensureDir(pd) && SUCCEEDED(webview_->QueryInterface(IID_PPV_ARGS(&v3))) && v3)
        v3->SetVirtualHostNameToFolderMapping(kTrProxyHost, pd.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
}

// ---- the dialogs ---------------------------------------------------------------------------------------------
// Files (MULTI-select) or ONE folder. EVERY container the helper's decoder reads is offered, not only
// what the window can play: a file the window cannot play is still analysed, played as audio through
// the proxy, and gets its .srt.
std::vector<std::wstring> Bridge::pickMediaFiles(HWND owner, bool folder) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return out;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    if (folder) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Play a folder of videos or audio files");
    } else {
        dlg->SetOptions(opts | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Open videos or audio files");
        const COMDLG_FILTERSPEC filter[] = {
            { L"Video and audio files",
              L"*.mp4;*.m4v;*.mkv;*.webm;*.mov;*.avi;*.wmv;*.mpg;*.mpeg;*.ts;*.m2ts;*.mts;*.flv;*.3gp;"
              L"*.ogv;*.mxf;*.mp3;*.wav;*.m4a;*.aac;*.flac;*.ogg;*.oga;*.opus;*.mka;*.wma;*.aif;*.aiff;"
              L"*.aifc;*.ac3;*.eac3;*.amr" },
            { L"All files", L"*.*" },
        };
        dlg->SetFileTypes(2, filter);
        dlg->SetFileTypeIndex(1);
    }
    // A fixed GUID of our own: Windows then remembers the folder THIS dialog was last used in (the
    // user's films), separately from the export dialog. SuVtitle (2026-10-23): its own GUID, not AudioPB's
    // {7a3c9e52-4b1d-4f6e-9c0a-2d5e8b7f1a63} -- the two apps no longer share one dialog's memory.
    static const GUID kTrOpenDialog =
        { 0x5d1e7c33, 0x2a6f, 0x4b80, { 0xa4, 0x19, 0x6c, 0xe2, 0x0b, 0x93, 0x57, 0xd8 } };
    dlg->SetClientGuid(kTrOpenDialog);
    trDialogBusy_ = true;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(dlg->GetResults(&arr)) && arr) {
            DWORD n = 0;
            arr->GetCount(&n);
            for (DWORD i = 0; i < n && out.size() < pbplayer::kQueueMax; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(arr->GetItemAt(i, &item)) && item) {
                    PWSTR p = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { out.push_back(p); ::CoTaskMemFree(p); }
                    item->Release();
                }
            }
            arr->Release();
        }
    }
    trDialogBusy_ = false;
    dlg->Release();
    // The dialog lists files in its own order; a playlist is read in the NATURAL one.
    if (!folder) std::stable_sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return pbplayer::naturalLess(pbtrf::leafOf(a), pbtrf::leafOf(b));
    });
    return out;
}

std::wstring Bridge::pickSubtitleFile(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return std::wstring();
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Load a subtitle file");
    const COMDLG_FILTERSPEC filter[] = {
        { L"Subtitle files (*.srt, *.vtt, *.ass, *.ssa)", L"*.srt;*.vtt;*.ass;*.ssa" },
        { L"All files", L"*.*" },
    };
    dlg->SetFileTypes(2, filter);
    dlg->SetFileTypeIndex(1);
    // Next to the film being played: that is where its subtitles usually are.
    if (player_ && player_->loaded()) {
        IShellItem* item = nullptr;
        const std::wstring f = player_->session().folder;
        if (!f.empty() && SUCCEEDED(::SHCreateItemFromParsingName(f.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    std::wstring path;
    trDialogBusy_ = true;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; ::CoTaskMemFree(p); }
            item->Release();
        }
    }
    trDialogBusy_ = false;
    dlg->Release();
    return path;
}

// ---- U2 (02/11/2026): THE PLAYER'S OWN MODELS ("Import model...") --------------------------------------------------
// The user: "garder le petit menu de selection, avec une option 'Import model', pour les utilisateurs qui voudraient
// importer leur propre modele IA pour la traduction/transcription". app.txt MODEL / MODELUSE (AppConfig.h) hold the
// imported files -- REFERENCED IN PLACE, never copied (they weigh GBs) -- and the choice of each kind. The KIND is the
// file's HEADER's (TrPipe.h modelFileKind: a whisper.cpp model = transcription, a GGUF = translation), never its name. The
// page names a model by an id minted here (its 1-based place in its kind's list; 0 = SuVtitle's default), never by a path
// (D1). The EFFECTIVE choice is the chosen file when it is there, else the default -- the entry greyed "not found", the
// choice kept in app.txt for when the file comes back (a disk unplugged). A model that fails in a job: the core goes back
// to the default for that kind and tells us (onModelFailed), and app.txt forgets the choice.
static const wchar_t* modelKindWord(const std::string& kind) { return kind == "mt" ? L"translation" : L"transcription"; }

void Bridge::loadModels() {
    std::string text;
    const std::wstring p = appDataPath(L"app.txt");
    if (!p.empty() && pbtrf::readFileAll(p, text, 1ULL << 20)) models_->m = pbapp::parseModels(text);
    applyModels();
}

// app.txt's MODEL / MODELUSE lines = the lists now (main.cpp writes WINPOS / LIVEPOS, saveLiveConfig LIVEIN in the same
// file, on the same thread: each rewrites its own lines). An EMPTY READ IS NOT AN EMPTY FILE: one that exists but could not
// be read is left alone.
void Bridge::saveModels() {
    const std::wstring cfg = appDataPath(L"app.txt");
    if (cfg.empty()) return;
    std::string before;
    if (!pbtrf::readFileAll(cfg, before, 1ULL << 20) && ::GetFileAttributesW(cfg.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const std::string after = pbapp::packModels(before, models_->m);
    if (after != before) pbtrf::writeFileAvSafe(cfg, after);   // temp + rename with the antivirus retry
}

void Bridge::applyModels() {
    if (!player_) return;
    auto pickOf = [&](const std::string& kind) {
        pbplayer::Player::ModelPick k;
        const std::string& use = models_->m.use(kind);
        uint64_t size = 0, mtime = 0;
        bool isDir = false;
        const std::wstring w = utf8ToWide(use.c_str());
        if (!use.empty() && pbtrf::fileIdentity(w, size, mtime, &isDir) && !isDir) {
            k.path = use;
            k.ident = pbtrf::modelIdent(w, size, mtime);
        }
        return k;
    };
    player_->setModels(pickOf("asr"), pickOf("mt"));
}

// translatorModels{asr:{sel, items:[{id, name, path, found, def, size, inUse}]}, mt:{...}} -- id 0 = SuVtitle's own model
// (def); sel = the EFFECTIVE choice's id (the core's); found = the file is there (else greyed "not found", not choosable). U2b:
// size = its bytes (0 when gone: the removal dialog says it); inUse = an analysis uses it NOW (the core's modelInUse: its x is
// disabled, and handleModelRemove refuses it). Posted again whenever the models in use change (Host::modelsInUseChanged).
void Bridge::postModels() {
    if (!player_ || !models_) return;
    JsonW j;
    j.obj().kv(L"type", L"translatorModels");
    for (const char* kind : { "asr", "mt" }) {
        const std::vector<std::string>& list = models_->m.list(kind);
        const std::string& now = std::string(kind) == "mt" ? player_->mtModel().path : player_->asrModel().path;
        long long sel = 0;
        j.key(std::string(kind) == "mt" ? L"mt" : L"asr").obj();
        j.key(L"items").arr();
        j.obj().kv(L"id", 0LL).kv(L"name", std::string(kind) == "mt" ? L"Qwen3-4B" : L"large-v3-turbo")
               .kv(L"def", true).kv(L"found", true).end();
        for (size_t i = 0; i < list.size(); ++i) {
            const std::wstring w = utf8ToWide(list[i].c_str());
            uint64_t size = 0, mtime = 0;
            bool isDir = false;
            const bool found = pbtrf::fileIdentity(w, size, mtime, &isDir) && !isDir;
            if (found && !now.empty() && pbapp::samePathCi(list[i], now)) sel = static_cast<long long>(i + 1);
            j.obj().kv(L"id", static_cast<long long>(i + 1)).kv(L"name", pbtrf::leafOf(w)).kv(L"path", w)
                   .kv(L"found", found).kv(L"def", false).kv(L"size", static_cast<long long>(found ? size : 0))
                   .kv(L"inUse", player_->modelInUse(list[i])).end();
        }
        j.end();
        j.kv(L"sel", sel);
        j.end();
    }
    j.end();
    postToTranslatorView(j.take());
}

std::wstring Bridge::pickModelFile(HWND owner) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg)
        return std::wstring();
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Import a model -- whisper.cpp (.bin) for transcription, GGUF (.gguf) for translation");
    const COMDLG_FILTERSPEC filter[] = {
        { L"AI models (*.bin, *.gguf)", L"*.bin;*.gguf" },
        { L"whisper.cpp models, transcription (*.bin)", L"*.bin" },
        { L"GGUF models, translation (*.gguf)", L"*.gguf" },
        { L"All files", L"*.*" },
    };
    dlg->SetFileTypes(4, filter);
    dlg->SetFileTypeIndex(1);
    std::wstring path;
    trDialogBusy_ = true;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { path = p; ::CoTaskMemFree(p); }
            item->Release();
        }
    }
    trDialogBusy_ = false;
    dlg->Release();
    return path;
}

// "Import model..." -- the dialog (owned by the player's window), then the file's HEADER decides: a whisper.cpp model joins
// the transcription models, a GGUF the translation ones, anything else is refused in one sentence. An import is CHOSEN at
// once (that is what it was imported for); importing a file already listed just chooses it.
void Bridge::handleModelImport() {
    if (trDialogBusy_ || !player_ || !models_) return;
    const std::wstring path = pickModelFile(dialogOwner());
    if (path.empty()) return;
    const std::wstring leaf = pbtrf::leafOf(path);
    std::string why;
    const pbt::trpipe::ModelKind k = pbt::trpipe::modelFileKind(path, &why);
    if (k == pbt::trpipe::ModelKind::None) {
        postTranslatorFileNotice(L"\"" + leaf + L"\" cannot be used as a model: " + utf8ToWide(why.c_str()) + L".");
        return;
    }
    const std::string kind = k == pbt::trpipe::ModelKind::Mt ? "mt" : "asr";
    const std::string u = wideToUtf8(path);
    if (!pbt::trpipe::modelPathOk(u)) {
        postTranslatorFileNotice(L"\"" + leaf + L"\" cannot be imported from where it is: its path is too long, or of a form "
                                 L"SuVtitle does not take. Move it to a folder with a shorter path.");
        return;
    }
    if (!pbapp::addImportedModel(models_->m, kind, u)) {
        postTranslatorFileNotice(L"SuVtitle keeps 16 imported " + std::wstring(modelKindWord(kind)) + L" models at most.");
        return;
    }
    for (const auto& x : models_->m.list(kind)) if (pbapp::samePathCi(x, u)) models_->m.use(kind) = x;   // (its listed spelling)
    saveModels();
    applyModels();
    postModels();
    postTranslatorFileNotice(L"\"" + leaf + L"\" imported: SuVtitle uses it for " + modelKindWord(kind)
                             + L" from the next Translate.", true);
}

// A pick in the menu: id 0 = SuVtitle's own model; else the kind's id-th imported file -- checked again (it is still there,
// still a model of that kind: the user may have replaced it) before it is chosen.
void Bridge::handleModelPick(const std::wstring& kindW, long long id) {
    if (!player_ || !models_ || (kindW != L"asr" && kindW != L"mt")) return;
    const std::string kind = kindW == L"mt" ? "mt" : "asr";
    const std::vector<std::string>& list = models_->m.list(kind);
    if (id < 0 || id > static_cast<long long>(list.size())) return;
    if (id == 0) {
        models_->m.use(kind).clear();
    } else {
        const std::string p = list[static_cast<size_t>(id - 1)];
        const std::wstring w = utf8ToWide(p.c_str());
        std::string why;
        const pbt::trpipe::ModelKind k = pbt::trpipe::modelFileKind(w, &why);
        const pbt::trpipe::ModelKind want = kind == "mt" ? pbt::trpipe::ModelKind::Mt : pbt::trpipe::ModelKind::Asr;
        if (k != want) {
            if (k != pbt::trpipe::ModelKind::None) why = "it is not a " + std::string(kind == "mt" ? "translation" : "transcription")
                                                        + " model any more";
            postTranslatorFileNotice(L"\"" + pbtrf::leafOf(w) + L"\" cannot be used for " + modelKindWord(kind) + L": "
                                     + utf8ToWide(why.c_str()) + L".");
            postModels();                                        // (the menu shows the choice that stands)
            return;
        }
        models_->m.use(kind) = p;
    }
    saveModels();
    applyModels();
    postModels();
}

// ---- U2b (02/11/2026, docs/PROGRESS_U2b.md): REMOVING AN IMPORTED MODEL -- the user: "une boite de dialogue qui s'ouvre et
// demande si on supprime le model en plus, pour eviter de laisser trainer des models qui ne sont pas utilises" ----------------
// A file held for DELETE (or WRITE) by another program: a sharing / lock violation when opening it with every share mode open.
static bool trFileHeld(const std::wstring& path, DWORD access) {
    HANDLE h = ::CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) { ::CloseHandle(h); return false; }
    const DWORD e = ::GetLastError();
    return e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION;
}
// A path as Windows resolves it (junctions, mapped names, 8.3 aliases followed); "" when it cannot be opened.
static std::wstring trFinalPath(const std::wstring& p) {
    HANDLE h = ::CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::wstring();
    std::wstring out(1024, L'\0');
    DWORD n = ::GetFinalPathNameByHandleW(h, &out[0], static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (n >= out.size()) {
        out.assign(static_cast<size_t>(n) + 1, L'\0');
        n = ::GetFinalPathNameByHandleW(h, &out[0], static_cast<DWORD>(out.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    }
    ::CloseHandle(h);
    if (n == 0 || n >= out.size()) return std::wstring();
    out.resize(n);
    return out;
}
// SuVtitle's OWN files are never sent away, even one the user imported as a model (the engine needs them: the default models,
// the Live presets): a file inside the app's folder (the bundle: <exe dir>\models\ and the rest), the dev payload
// (SUVTITLE_PAYLOAD) or engine.txt's MODELS folder (the engine's own config, READ only). Compared on FINAL paths: a junction,
// a mapped name or an 8.3 alias of one of those folders is that folder.
static bool trOwnModelFile(const std::wstring& path, const std::wstring& engineCfg) {
    const std::wstring file = trFinalPath(path);
    if (file.empty()) return false;
    std::vector<std::wstring> dirs;
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::wstring e(exe, n);
        const size_t cut = e.find_last_of(L"\\/");
        if (cut != std::wstring::npos) dirs.push_back(e.substr(0, cut));          // the app's folder (the bundle)
    }
    wchar_t env[2048] = {};
    const DWORD m = ::GetEnvironmentVariableW(L"SUVTITLE_PAYLOAD", env, 2048);
    if (m > 0 && m < 2048) dirs.push_back(std::wstring(env, m));                   // the dev payload
    std::string cfg;
    if (!engineCfg.empty() && pbtrf::readFileAll(engineCfg, cfg, 1ULL << 20))
        for (const auto& l : pbapp::lines(cfg))
            if (l.size() > 7 && l.compare(0, 7, "MODELS\t") == 0) dirs.push_back(utf8ToWide(l.substr(7).c_str()));   // engine.txt
    for (const auto& d : dirs) {
        std::wstring f = trFinalPath(d);
        if (f.empty()) continue;
        if (f.back() != L'\\') f += L'\\';
        if (file.size() > f.size()
            && ::CompareStringOrdinal(file.c_str(), static_cast<int>(f.size()), f.c_str(), static_cast<int>(f.size()), TRUE) == CSTR_EQUAL)
            return true;
    }
    return false;
}
namespace {
// The shell's word on the one delete it ran (PostDeleteItem): RECYCLED = an item newly created in the Recycle Bin.
struct RecycleSink : public IFileOperationProgressSink {
    HRESULT hr = S_OK;
    bool done = false, recycled = false;
    // IUnknown -- a stack object that outlives the operation (Unadvise before it goes): no counting
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
    IFACEMETHODIMP_(ULONG) Release() override { return 1; }
    IFACEMETHODIMP StartOperations() override { return S_OK; }
    IFACEMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    IFACEMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT hrDelete, IShellItem* newlyCreated) override {
        done = true;
        hr = hrDelete;
        recycled = SUCCEEDED(hrDelete) && newlyCreated != nullptr;
        return S_OK;
    }
    IFACEMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    IFACEMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override { return S_OK; }
    IFACEMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    IFACEMETHODIMP ResetTimer() override { return S_OK; }
    IFACEMETHODIMP PauseTimer() override { return S_OK; }
    IFACEMETHODIMP ResumeTimer() override { return S_OK; }
};
}   // namespace
// U2b fix round (the director, 02/11): a file whose drive or folder CANNOT BE REACHED -- its parent missing, the drive unplugged
// (or a card reader empty), the share offline. Such a model is NOT gone: it stays listed (it comes back with its drive).
static bool trUnreachable(DWORD e) {
    return e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_DRIVE || e == ERROR_NOT_READY || e == ERROR_BAD_NETPATH
        || e == ERROR_DEV_NOT_EXIST || e == ERROR_NETNAME_DELETED || e == ERROR_BAD_NET_NAME || e == ERROR_NETWORK_UNREACHABLE;
}
// TRULY gone: "not found" (`e`, GetFileAttributesW's error) in a folder that IS there -- never a missing parent read as gone.
static bool trTrulyGone(const std::wstring& path, DWORD e) {
    if (e != ERROR_FILE_NOT_FOUND) return false;
    const std::wstring dir = pbtrf::dirOf(path);             // ("C:\x.gguf" -> "C:\": a drive root keeps its backslash)
    if (dir.empty()) return false;
    const DWORD a = ::GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
// THE RECYCLE BIN -- the ONE way SuVtitle sends a file of the user's away, through the shell's own copy engine (IFileOperation,
// owned by the player's window); nothing else ever deletes a model file: no DeleteFile, no std::filesystem::remove, no
// SHFileOperation. FOFX_RECYCLEONDELETE + FOF_ALLOWUNDO: to the Recycle Bin, restorable; FOF_NOCONFIRMATION: our dialog already
// asked; FOF_WANTNUKEWARNING: when Windows would DESTROY it instead (no Recycle Bin on that drive, a file larger than the bin),
// WINDOWS' OWN WARNING appears and decides (it partly overrides FOF_NOCONFIRMATION); FOF_NOERRORUI: no error box (the page says
// it). Returns 1 = in the Recycle Bin, 2 = destroyed after the user accepted Windows' warning, 0 = not deleted (`why`: the end
// of one sentence). MEASURED (U2b, D): a model the engine keeps loaded does not block it (llama.cpp's mapping lets it move).
static int trRecycleFile(HWND owner, const std::wstring& path, std::wstring& why) {
    why.clear();
    IFileOperation* op = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) || !op) {
        why = L"Windows' file operation could not be started.";
        return 0;
    }
    IShellItem* item = nullptr;
    RecycleSink sink;
    DWORD cookie = 0;
    bool advised = false;
    HRESULT hr = ::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (SUCCEEDED(hr)) hr = op->SetOwnerWindow(owner);
    if (SUCCEEDED(hr)) hr = op->SetOperationFlags(FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_WANTNUKEWARNING | FOF_NOERRORUI
                                                  | FOFX_RECYCLEONDELETE);
    if (SUCCEEDED(hr)) advised = SUCCEEDED(op->Advise(&sink, &cookie));
    if (SUCCEEDED(hr)) hr = op->DeleteItem(item, nullptr);
    if (SUCCEEDED(hr)) hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    if (advised) op->Unadvise(cookie);
    if (item) item->Release();
    op->Release();
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // gone: where to is the shell's word (no word: the flags asked for the Recycle Bin, and a destruction asks first)
        return (!advised || !sink.done || sink.recycled) ? 1 : 2;
    }
    const HRESULT e = FAILED(sink.hr) ? sink.hr : hr;
    // (in use FIRST: a failed item can also count as "aborted" -- the probe says what holds it)
    if (trFileHeld(path, DELETE)) why = L"it is in use by another program.";
    else if (trUnreachable(HRESULT_FACILITY(e) == FACILITY_WIN32 ? static_cast<DWORD>(HRESULT_CODE(e)) : 0))
        why = L"its drive or folder cannot be reached.";
    else if (aborted || e == HRESULT_FROM_WIN32(ERROR_CANCELLED)) why = L"you kept it when Windows asked.";
    else if (e == E_ACCESSDENIED) why = L"Windows refused (access denied).";
    else {
        wchar_t b[80] = {};
        ::swprintf(b, 80, L"Windows could not move it (error 0x%08lX).", static_cast<unsigned long>(e));
        why = b;
    }
    return 0;
}
// translatorModelRemove{kind, id, recycle} -- the page's dialog answered. The PATH is THE LIST'S (app.txt's MODEL lines,
// models_->m: the kind's id-th imported file), never the page's; refused: SuVtitle's own (id 0), an id the list does not have, a
// kind that is not asr / mt -- and a model an analysis uses NOW (the core's modelInUse: the page's x is disabled then; this is
// the double guard). "Remove from the list": THE LINE, whatever state the file is in. "Remove and delete the file" (U2b fix
// round, the director 02/11 -- the user: "pour eviter de laisser trainer des modeles qui ne sont pas utilises": never half done):
// the RECYCLE BIN FIRST (trRecycleFile); the line goes only when the file WENT (in the bin, or destroyed after Windows' own
// warning was accepted -- and then truly gone from a folder that is there), is TRULY gone already (trTrulyGone), or is in one of
// SuVtitle's own folders (trOwnModelFile: the line goes, the file stays); anything else is a FAILURE and NOTHING changes --
// still listed, still chosen if it was (an unplugged drive's model must not come back orphaned): the menu posted again, ONE
// sentence says why. The line = removeImportedModel + saveModels (the AV-safe write) + applyModels (the choice back to the
// default when it was that model) + the menu; ONE notice says what was done.
void Bridge::handleModelRemove(const std::wstring& kindW, long long id, bool recycle) {
    if (trDialogBusy_ || !player_ || !models_ || (kindW != L"asr" && kindW != L"mt")) return;
    const std::string kind = kindW == L"mt" ? "mt" : "asr";
    const std::vector<std::string>& list = models_->m.list(kind);
    if (id < 1 || id > static_cast<long long>(list.size())) { postModels(); return; }   // (id 0: SuVtitle's own -- never removed)
    const std::string path = list[static_cast<size_t>(id - 1)];                         // THE LIST'S path, never the page's
    const std::wstring w = utf8ToWide(path.c_str()), leaf = pbtrf::leafOf(w);
    if (player_->modelInUse(path)) {
        postTranslatorFileNotice(L"\"" + leaf + L"\" is being used by an analysis: cancel it, or wait for it to end, to remove it.");
        postModels();
        return;
    }
    std::wstring after;                                      // what the notice says of the file (asked to delete it)
    if (recycle) {
        std::wstring why;
        const DWORD at = ::GetFileAttributesW(w.c_str());
        const DWORD err = at == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : 0;
        if (at == INVALID_FILE_ATTRIBUTES && trTrulyGone(w, err)) {
            after = L" Its file was not found any more: nothing was deleted.";
        } else if (at == INVALID_FILE_ATTRIBUTES) {
            why = trUnreachable(err) ? std::wstring(L"its drive or folder cannot be reached.")
                                     : L"Windows cannot reach it (error " + std::to_wstring(err) + L").";
        } else if (at & FILE_ATTRIBUTE_DIRECTORY) {
            why = L"a folder is there now, not the model file.";
        } else if (trOwnModelFile(w, appDataPath(L"engine.txt"))) {
            after = L" Its file is in one of SuVtitle's own folders: it stays where it is (SuVtitle never deletes its own files).";
        } else {
            trDialogBusy_ = true;                            // (Windows' own warning is modal: no other dialog meanwhile)
            const int r = trRecycleFile(dialogOwner(), w, why);
            trDialogBusy_ = false;
            const DWORD errAfter = ::GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES ? ::GetLastError() : 0;
            if (r != 0 && !trTrulyGone(w, errAfter))         // (said gone -- but not gone from a folder that is there: its drive left)
                why = trUnreachable(errAfter) ? L"its drive or folder cannot be reached." : L"Windows could not move it.";
            else if (r == 1) after = L" Its file is in the Recycle Bin.";
            else if (r == 2) after = L" Windows could not put its file in the Recycle Bin and deleted it, as you confirmed.";
        }
        if (after.empty()) {                                 // a FAILURE: NOTHING changes (still listed, still chosen if it was)
            postModels();
            postTranslatorFileNotice(L"The file was not deleted: " + why + L" \"" + leaf
                                     + L"\" stays in the list: try again, or choose Remove from the list.");
            return;
        }
    }
    const bool wasChosen = pbapp::samePathCi(models_->m.use(kind), path);
    pbapp::removeImportedModel(models_->m, kind, path);
    saveModels();
    applyModels();
    postModels();
    std::wstring said = L"\"" + leaf + L"\" removed from the models.";
    if (wasChosen)
        said += std::wstring(L" SuVtitle uses its own ") + modelKindWord(kind) + L" model ("
              + (kind == "mt" ? L"Qwen3-4B" : L"large-v3-turbo") + L") again.";
    postTranslatorFileNotice(said + after, true);
}

// The core went back to the default for `kind` (Host::modelFailed): app.txt forgets that choice -- when it still is the
// one -- and the menu follows. U2b: the notice ONCE -- when the core says so (`say`: THIS failure made the choice fall back,
// and the strip does not say it: a file not on screen, or one a batch redoes at once); `redone` = the file the batch redoes.
void Bridge::onModelFailed(const std::string& kind, const std::string& path, const std::wstring& sentence, bool say,
                           const std::wstring& redone) {
    if (!models_) return;
    if (pbapp::samePathCi(models_->m.use(kind), path)) {
        models_->m.use(kind).clear();
        saveModels();
    }
    postModels();
    if (say) postTranslatorFileNotice(sentence + L" SuVtitle goes back to its own " + modelKindWord(kind) + L" model"
                                      + (redone.empty() ? std::wstring(L".") : L" and redoes \"" + redone + L"\" with it."));
}

// ---- opening, dropping, scanning -------------------------------------------------------------------------------
// Open = play (the files go right after the current item, the first starts now); Add = append to the queue.
// SuVtitle: every request comes from THE window. AudioPB's desk card "Translate a file..." -- which forced translation
// on (`force`) and brought the translator window up -- went with the desk.
void Bridge::handlePlayerOpen(const std::wstring& what, bool play, const std::wstring& quality) {
    if (trDialogBusy_ || !player_) return;
    if (!quality.empty()) player_->setQuality(u8(trQuality(quality)));
    const bool folder = what == L"folder";
    std::vector<std::wstring> paths = pickMediaFiles(dialogOwner(), folder);
    if (paths.empty()) return;                 // Cancel: nothing to say
    if (folder) startPlayerScan(paths, {}, {}, play, false);
    else if (play) player_->open(paths, false);
    else player_->add(paths);
}

// WG: SuVtitle AS A WINDOWS PLAYER -- the paths of --play (Explorer's double-click, "Open with", another SuVtitle.exe
// forwarding them through WM_COPYDATA). Each one checked (TrLaunch.h classify): folders are scanned, media files
// play (the first one now), a subtitle file goes to what plays; anything else is left out, and said.
void Bridge::playPaths(const std::vector<std::wstring>& paths) {
    if (!player_) return;
    startup();                                            // (the queue first: the paths join it, never under it)
    std::vector<std::wstring> folders, media, subs;
    std::vector<pbplayer::Player::Known> known;
    size_t refused = 0;
    for (const auto& p : paths) {
        switch (pblaunch::classify(p)) {
            case pblaunch::Kind::Folder:   folders.push_back(p); break;
            case pblaunch::Kind::Subtitle: subs.push_back(p); break;
            case pblaunch::Kind::Media: {
                uint64_t s = 0, m = 0;
                if (pbtrf::fileIdentity(p, s, m)) { media.push_back(p); known.push_back(pbplayer::Player::Known{ s, m, true }); }
                else ++refused;
                break;
            }
            default: ++refused;
        }
    }
    if (!folders.empty()) startPlayerScan(folders, media, subs, true, false);
    else {
        if (!media.empty()) player_->open(media, false, &known);
        if (!subs.empty()) { player_->userSubFile(); loadExtSub(subs.front(), 0, 0); }   // (RV4-1: the viewer's)
    }
    if (refused)
        player_->notice(std::to_wstring(refused) + (refused == 1 ? L" item was" : L" items were")
                        + L" not a video, audio or subtitle file (or not found) and " + (refused == 1 ? L"was" : L"were") + L" left out.");
    syncNative();   // 1.1.0 NATIVE: what --play opened plays in mpv
}

// THE FREEZE WATCH (25/10, the user's full-screen report on 1.0.0 -- not reproduced on the test screen: index.html): the page
// reports a freeze of its own; this writes ONE line of %LOCALAPPDATA%\SuVtitle\player.log with what the app knows besides --
// its window (full screen, foreground, minimised) and the engine's work (an analysis in flight). The page's words are
// checked against closed lists (anything else is written "?"; an unknown kind writes nothing). A page gone wrong cannot
// fill the disk: 60 lines a minute at most, and a log over 512 KB starts again, truncated in place (no rename: no
// antivirus window).
void Bridge::handlePlayerDiag(const std::wstring& json) {
    std::wstring kind, render, sub;
    long long ms = 0;
    bool full = false, playing = false, shown = true;
    jsonFindString(json, L"kind", kind);
    jsonFindString(json, L"render", render);
    jsonFindString(json, L"sub", sub);
    jsonFindNumber(json, L"ms", ms);
    jsonFindBool(json, L"full", full);
    jsonFindBool(json, L"playing", playing);
    jsonFindBool(json, L"shown", shown);
    const auto pick = [](const std::wstring& v, std::initializer_list<const wchar_t*> known) -> std::string {
        for (const wchar_t* k : known) if (v == k) return wideToUtf8(v);
        return "?";
    };
    const std::string k = pick(kind, { L"frames", L"input", L"hidden", L"visible" });
    if (k == "?") return;
    const ULONGLONG now = ::GetTickCount64();
    if (now - diagMinute_ >= 60000) { diagMinute_ = now; diagLines_ = 0; }
    if (++diagLines_ > 60) return;
    const HWND h = hostWnd_.load();
    const bool fg = h && ::GetForegroundWindow() == h, iconic = h && ::IsIconic(h), visible = h && ::IsWindowVisible(h);
    std::string engine = "idle";
    if (player_ && !player_->jobs().empty())
        engine = player_->jobs().front().pre ? std::string("pre-analysing") : player_->jobs().front().phase;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char line[400];
    std::snprintf(line, sizeof(line),
                  "%04u-%02u-%02u %02u:%02u:%02u.%03u [SuVtitle] page %s %lld ms | page: %s, %s, %s, render %s, subtitles %s"
                  " | window: %s%s%s%s | engine %s\r\n",
                  (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay, (unsigned)st.wHour, (unsigned)st.wMinute,
                  (unsigned)st.wSecond, (unsigned)st.wMilliseconds, k.c_str(), ms < 0 ? 0LL : (ms > 86400000LL ? 86400000LL : ms),
                  full ? "full screen" : "windowed", playing ? "playing" : "paused", shown ? "shown" : "not shown",
                  pick(render, { L"blend", L"canvas", L"plain", L"native" }).c_str(),   // (1.1.0 NATIVE: mpv draws it)
                  pick(sub, { L"audiopb", L"ext", L"off" }).c_str(),
                  trWin_.fullscreen ? "full screen" : "windowed", fg ? ", foreground" : "", iconic ? ", minimised" : "",
                  visible ? "" : ", invisible", engine.c_str());
    wchar_t la[MAX_PATH] = {};
    if (!::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) return;
    const std::wstring dir = std::wstring(la) + L"\\SuVtitle", path = dir + L"\\player.log";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool big = ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
                     && (fa.nFileSizeHigh || fa.nFileSizeLow > 512u * 1024u);
    HANDLE f = ::CreateFileW(path.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const std::string out = (big ? std::string("(player.log started again: it was over 512 KB)\r\n") : std::string()) + line;
    DWORD wr = 0;
    ::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &wr, nullptr);
    ::CloseHandle(f);
}

// WG: "Set as default player..." -- Windows lets no application make itself the default (the choice is the user's,
// in Settings, protected by a hash): SuVtitle REGISTERS for this user as a player for every media type it plays
// (TrAssoc.h: its ProgIDs, "Open with", Capabilities, RegisteredApplications) and opens Settings on itself. A test
// instance registers under its own test root and opens nothing.
void Bridge::handleSetDefaultPlayer() {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const bool test = pblaunch::isTestInstance();
    const std::wstring root = test ? L"Software\\SuVtitle-Test\\" + pblaunch::testTag() : std::wstring(L"Software");
    std::wstring err;
    if (!exe[0] || !pbassoc::apply(HKEY_CURRENT_USER, pbassoc::entries(exe, root), err)) {
        postTranslatorFileNotice(L"SuVtitle could not register itself as a player (" + err + L").");
        return;
    }
    if (test) {                                       // (nothing the shell reads: no broadcast either)
        postTranslatorFileNotice(L"Registered under HKEY_CURRENT_USER\\" + root + L" (a test instance: Settings not opened -- "
                                 + std::wstring(pbassoc::kSettingsUri) + L").", true);
        return;
    }
    pbassoc::notifyShell();
    const HINSTANCE r = ::ShellExecuteW(nullptr, L"open", pbassoc::kSettingsUri, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(r) <= 32) ::ShellExecuteW(nullptr, L"open", pbassoc::kSettingsFallback, nullptr, nullptr, SW_SHOWNORMAL);
    postTranslatorFileNotice(L"SuVtitle is registered: in Settings > Default apps, choose it for the file types you want "
                             L"(.mp4, .mkv, .mp3\x2026). It is also in every video's \"Open with\" menu.", true);
}

// A drop on the window: files, FOLDERS (measured: a folder's File object carries its path, STEP0 M4)
// and subtitle files, in any mix. target "queue" = the queue panel (append); anything else = the
// player (play now). Nothing about the current file changes for what is refused.
void Bridge::handleTranslatorFileDrop(const std::vector<std::wstring>& paths, const std::wstring& quality,
                                      const std::wstring& target) {
    if (!player_) return;
    if (paths.empty()) {
        postTranslatorFileNotice(L"That could not be read as a file. Drop videos, audio files or folders, "
                                 L"or use Open files\x2026");
        return;
    }
    if (!quality.empty()) player_->setQuality(u8(trQuality(quality)));
    std::vector<std::wstring> folders, media, subs;
    std::vector<pbplayer::Player::Known> known;          // the identity read here, not read again (P2)
    size_t other = 0;
    for (const auto& p : paths) {
        uint64_t s = 0, m = 0;
        bool isDir = false;
        if (!pbtrf::fileIdentity(p, s, m, &isDir)) { ++other; continue; }
        const std::wstring ext = pbtrf::extOf(pbtrf::leafOf(p));
        if (isDir) folders.push_back(p);
        else if (pbtrf::isMediaExt(ext)) { media.push_back(p); known.push_back(pbplayer::Player::Known{ s, m, true }); }
        else if (pbsubr::isSubtitleExt(ext)) subs.push_back(p);
        else ++other;
    }
    const bool play = target != L"queue";
    if (!folders.empty()) {
        // the subtitle files go WITH the scan: applied after its files are in (C8)
        startPlayerScan(folders, media, subs, play, false);
    } else {
        if (!media.empty()) { if (play) player_->open(media, false, &known); else player_->add(media, &known); }
        if (!subs.empty()) { player_->userSubFile(); loadExtSub(subs.front(), 0, 0); }   // (RV4-1: the viewer's)
    }
    if (folders.empty() && media.empty() && subs.empty())
        postTranslatorFileNotice(L"That is not a video, audio or subtitle file.");
    else if (other)
        postTranslatorFileNotice(std::to_wstring(other) + (other == 1 ? L" item was" : L" items were")
                                 + L" not a video, audio or subtitle file and " + (other == 1 ? L"was" : L"were")
                                 + L" left out.", true);
}

// A folder (or several) -> its media files, on a WORKER thread: a drop of a whole drive or a slow
// share must not freeze both windows (the scan is capped anyway: pbplayer::ScanCaps). The result is
// handed back through scanBox_ and the translator's wake (kWmTranslatorPoll -> pumpTranslator).
void Bridge::startPlayerScan(std::vector<std::wstring> folders, std::vector<std::wstring> files,
                             std::vector<std::wstring> subs, bool play, bool force) {
    if (!scanBox_ || folders.empty()) return;
    postTranslatorFileNotice(L"Looking for videos and audio files in " + pbtrf::leafOf(folders.front())
                             + (folders.size() > 1 ? L" and " + std::to_wstring(folders.size() - 1) + L" more" : std::wstring())
                             + L"\x2026", true);
    std::shared_ptr<ScanBox> box = scanBox_;
    const HWND h = hostWnd_.load();
    std::thread([box, folders, files, subs, play, force, h] {
        ScanBox::Done d;
        d.play = play;
        d.force = force;
        d.subs = subs;
        d.files = files;                       // the files dropped with the folders come first
        for (const auto& f : files) {          // their identity, read HERE (the worker), not on the UI thread
            pbplayer::Player::Known k;
            k.ok = pbtrf::fileIdentity(f, k.size, k.mtime);
            d.known.push_back(k);
        }
        for (const auto& f : folders) {
            pbplayer::ScanResult r;
            pbplayer::ScanCaps caps;
            caps.maxMedia = d.files.size() < pbplayer::kQueueMax ? pbplayer::kQueueMax - d.files.size() : 0;
            if (caps.maxMedia == 0) { d.truncated = true; break; }
            pbplayer::scanFolder(f, caps, r, &box->cancel);
            d.files.insert(d.files.end(), r.files.begin(), r.files.end());
            for (size_t i = 0; i < r.files.size(); ++i)   // from the listing itself (P2)
                d.known.push_back(pbplayer::Player::Known{ r.sizes[i], r.mtimes[i], true });
            d.truncated = d.truncated || r.truncated;
            if (box->cancel.load()) return;
        }
        {
            std::lock_guard<std::mutex> lk(box->mx);
            box->done.push_back(std::move(d));
        }
        if (h) ::PostMessageW(h, kWmTranslatorPoll, 0, 0);
    }).detach();
}

// WH: the answers of the track workers, to the core (UI thread).
void Bridge::drainPlayerTracks() {
    if (!trackBox_ || !player_) return;
    std::vector<TrackBox::Probe> probes;
    std::vector<TrackBox::Sub> subs;
    {
        std::lock_guard<std::mutex> lk(trackBox_->mx);
        probes.swap(trackBox_->probes);
        subs.swap(trackBox_->subs);
    }
    std::vector<TrackBox::Tc> tcs;                       // U3: the source timecodes read
    {
        std::lock_guard<std::mutex> lk(trackBox_->mx);
        tcs.swap(trackBox_->tcs);
    }
    for (auto& p : probes) player_->onTracks(p.req, p.info);
    for (auto& s : subs) player_->onSubExtracted(s.req, s.res);
    for (auto& t : tcs) player_->onTimecode(t.req, t.tc);
}

void Bridge::drainPlayerScans() {
    if (!scanBox_ || !player_) return;
    std::vector<ScanBox::Done> v;
    {
        std::lock_guard<std::mutex> lk(scanBox_->mx);
        v.swap(scanBox_->done);
    }
    for (auto& d : v) {
        if (d.files.empty()) {
            postTranslatorFileNotice(L"No video or audio file was found there.");
            if (!d.subs.empty()) { player_->userSubFile(); loadExtSub(d.subs.front(), 0, 0); }   // nothing new plays: the current file's
            continue;
        }
        if (d.play) player_->open(d.files, d.force, &d.known);
        else player_->add(d.files, &d.known);
        // the subtitle file dropped WITH the folder: for the file that plays NOW (C8) -- the viewer's (RV4-1)
        if (!d.subs.empty()) { player_->userSubFile(); loadExtSub(d.subs.front(), 0, 0); }
        if (d.truncated)
            postTranslatorFileNotice(L"That folder is very large: the first " + std::to_wstring(d.files.size())
                                     + L" files were taken.", true);
    }
}

// ---- subtitle files ----------------------------------------------------------------------------------------------
void Bridge::handlePlayerSubLoad() {
    if (trDialogBusy_ || !player_) return;
    if (!player_->loaded()) { postTranslatorFileNotice(L"Open a video first, then its subtitles."); return; }
    const std::wstring path = pickSubtitleFile(dialogOwner());
    if (!path.empty()) { player_->userSubFile(); loadExtSub(path, 0, 0); }   // (RV4-1: the viewer's)
}

// Read, decode (SubtitleReader.h: BOM, UTF-16, UTF-8, MLang + the GBK/Big5 tie-break, or the code
// page the user forced) and parse a subtitle file, then hand it to the window in chunks of runs
// (text + italic/bold/underline flags: the page builds spans by textContent -- no markup crosses).
// `id` = 0: a new file (a fresh id is minted); else the same file again (another encoding).
void Bridge::loadExtSub(const std::wstring& path, uint64_t id, unsigned codepage) {
    if (!player_ || !player_->loaded()) { postTranslatorFileNotice(L"Open a video first, then its subtitles."); return; }
    uint64_t size = 0, mtime = 0;
    bool isDir = false;
    if (!pbtrf::fileIdentity(path, size, mtime, &isDir) || isDir) {
        postTranslatorFileNotice(L"That subtitle file could not be read.");
        return;
    }
    if (size > pbsubr::kMaxBytes) { postTranslatorFileNotice(L"That subtitle file is too large (over 20 MB)."); return; }
    std::string bytes;
    if (!pbtrf::readFileAll(path, bytes, pbsubr::kMaxBytes)) { postTranslatorFileNotice(L"That subtitle file could not be read."); return; }
    const pbsubr::Result r = pbsubr::parse(bytes, codepage);
    if (!r.ok) { postTranslatorFileNotice(w16(r.err)); return; }
    if (!id) id = player_->newSubId();
    extSubPaths_[id] = path;
    // WG: the file becomes the EDITABLE document (an edited one before it is written first: never lost -- and if it
    // cannot be written anywhere, SAID before it goes: review #8)
    dropExtDoc();
    extDoc_ = std::make_unique<ExtDoc>();
    ExtDoc& d = *extDoc_;
    d.id = id;
    d.source = path;
    d.format = r.format;
    d.codepage = r.codepage;
    d.encName = pbsubr::encodingName(r.codepage);
    d.forced = r.forced;
    d.truncated = r.truncated;
    d.lossless = r.format != pbsubr::Format::Ass && !r.dropped && !r.fixed && !r.skipped && !r.truncated;
    d.srcSize = size; d.srcMtime = mtime;
    for (const auto& c : r.cues) {
        TrExtCue x;
        x.i = d.nextI++;
        x.s = c.startMs; x.e = c.endMs;
        x.runs = c.runs;
        x.text = trRunsToTagged(c.runs);
        d.cues.push_back(std::move(x));
    }
    player_->setExtSub(id, extDocChunks());
}

// The document's parts, as playerSubs messages (<= kTrSubChunk cues each): each cue with its id and its markup.
std::vector<std::wstring> Bridge::extDocChunks() {
    std::vector<std::wstring> chunks;
    if (!extDoc_) return chunks;
    const ExtDoc& d = *extDoc_;
    const size_t parts = d.cues.empty() ? 1 : (d.cues.size() + kTrSubChunk - 1) / kTrSubChunk;
    for (size_t part = 0; part < parts; ++part) {
        JsonW j;
        j.obj();
        j.kv(L"type", L"playerSubs");
        j.kv(L"id", static_cast<long long>(d.id));
        j.kv(L"name", pbtrf::leafOf(d.path.empty() ? d.source : d.path));
        j.kv(L"enc", static_cast<long long>(d.codepage));
        j.kv(L"encName", w16(d.encName));
        j.kv(L"forced", d.forced);
        j.kv(L"truncated", d.truncated);
        j.kv(L"part", static_cast<long long>(part));
        j.kv(L"parts", static_cast<long long>(parts));
        j.key(L"cues").arr();
        for (size_t i = part * kTrSubChunk; i < d.cues.size() && i < (part + 1) * kTrSubChunk; ++i) {
            const auto& c = d.cues[i];
            j.obj();
            j.kv(L"i", static_cast<long long>(c.i));
            j.kv(L"s", static_cast<long long>(c.s));
            j.kv(L"e", static_cast<long long>(c.e));
            j.key(L"r").arr();
            for (const auto& run : c.runs) { j.arr(); j.str(w16(run.text)); j.num(run.flags); j.end(); }
            j.end();
            j.end();
        }
        j.end();
        j.end();
        chunks.push_back(j.take());
    }
    return chunks;
}

// ---- WG: THE SUBTITLE EDITOR's bridge half --------------------------------------------------------------------------
// A SuVtitle set's edit goes to the core (TrPlayer.h editCues: it checks, applies, answers, persists). A subtitle
// FILE's edit is checked and applied HERE, on extDoc_: no new overlap (a file may already hold overlapping cues --
// two voices -- and an edit of one of them must stay possible), at least 0.1 s, the text re-read into runs by
// SubtitleReader's own markup reader; the file is written back 2 s after the last edit (tickExtDoc).
void Bridge::handleSubEdit(const std::wstring& json) {
    if (!player_) return;
    pbjson::Value v;
    const bool parsed = pbjson::parse(json, v, 16u << 20) && v.isObj();
    uint64_t rev = 0;
    if (parsed) v.u64(L"rev", rev);
    const pbjson::Value* rm = parsed ? v.get(L"remove") : nullptr;
    const pbjson::Value* ad = parsed ? v.get(L"add") : nullptr;
    const bool shaped = rm && ad && rm->isArr() && ad->isArr();
    if (parsed && v.str(L"target") == L"ext") {
        uint64_t id = 0;
        v.u64(L"id", id);
        std::wstring err;
        std::vector<uint64_t> removeIds;
        std::vector<TrExtCue> adds;
        bool ok = shaped;
        if (!ok) err = L"That edit could not be read.";
        for (size_t k = 0; ok && k < rm->a.size(); ++k) { uint64_t x = 0; if (!rm->a[k].asU64(x)) { ok = false; err = L"That edit could not be read."; } removeIds.push_back(x); }
        for (size_t k = 0; ok && k < ad->a.size(); ++k) {
            const pbjson::Value& o = ad->a[k];
            TrExtCue c;
            if (!o.isObj() || !o.u64(L"s", c.s) || !o.u64(L"e", c.e)) { ok = false; err = L"That edit could not be read."; break; }
            c.text = u8(o.str(L"text"));
            adds.push_back(std::move(c));
        }
        std::vector<TrExtCue> added;
        if (ok) ok = extDocEdit(id, removeIds, adds, added, v.flag(L"restore"), err);
        JsonW j;
        j.obj();
        j.kv(L"type", L"playerSubsEdited");
        j.kv(L"id", static_cast<long long>(id));
        j.kv(L"rev", static_cast<long long>(rev));
        j.kv(L"ok", ok);
        if (!ok) j.kv(L"err", err);
        j.key(L"remove").arr();
        if (ok) for (uint64_t x : removeIds) j.num(static_cast<long long>(x));
        j.end();
        j.key(L"add").arr();
        for (const auto& c : added) {
            j.obj();
            j.kv(L"i", static_cast<long long>(c.i));
            j.kv(L"s", static_cast<long long>(c.s));
            j.kv(L"e", static_cast<long long>(c.e));
            j.kv(L"t", w16(c.text));
            j.key(L"r").arr();
            for (const auto& run : c.runs) { j.arr(); j.str(w16(run.text)); j.num(run.flags); j.end(); }
            j.end();
            j.end();
        }
        j.end();
        j.end();
        postToTranslatorView(j.take());
        if (ok) player_->keepExtSub(id, extDocChunks());   // a page reloaded later gets the EDITED file back
        return;
    }
    uint64_t gen = 0;
    if (parsed) v.u64(L"gen", gen);
    std::vector<uint32_t> remove;
    std::vector<pbplayer::Player::EditCue> add;
    bool ok = shaped;
    for (size_t k = 0; ok && k < rm->a.size(); ++k) {
        uint64_t x = 0;
        if (!rm->a[k].asU64(x) || x > 0xFFFFFFFFULL) { ok = false; break; }
        remove.push_back(static_cast<uint32_t>(x));
    }
    for (size_t k = 0; ok && k < ad->a.size(); ++k) {
        const pbjson::Value& o = ad->a[k];
        pbplayer::Player::EditCue c;
        if (!o.isObj() || !o.u64(L"s", c.s) || !o.u64(L"e", c.e)) { ok = false; break; }
        c.lang = u8(o.str(L"lang"));
        c.dstLang = u8(o.str(L"dstLang"));
        c.src = u8(o.str(L"src"));
        c.dst = u8(o.str(L"dst"));
        c.pass = o.flag(L"pass");
        add.push_back(std::move(c));
    }
    if (!ok) { remove.clear(); add.clear(); }             // the core answers "Nothing to edit." (the page's wait ends)
    player_->editCues(gen, rev, remove, add);
}

bool Bridge::extDocEdit(uint64_t id, const std::vector<uint64_t>& removeIds, std::vector<TrExtCue>& adds,
                        std::vector<TrExtCue>& added, bool restore, std::wstring& err) {
    if (player_->isEmbeddedSub(id)) {                  // WH: read ONLY (the page says so too)
        err = L"This subtitle track is inside the video file: it can't be edited here.";
        return false;
    }
    if (!extDoc_ || extDoc_->id != id || !player_->loaded() || player_->session().extSubId != id) {
        err = L"This subtitle file changed meanwhile: nothing was edited.";
        return false;
    }
    ExtDoc& d = *extDoc_;
    if (removeIds.empty() && adds.empty()) { err = L"Nothing to edit."; return false; }
    if (removeIds.size() > pbsubr::kMaxCues || adds.size() > pbsubr::kMaxCues) { err = L"Too many subtitles in one edit."; return false; }
    std::set<uint64_t> rm(removeIds.begin(), removeIds.end());
    std::vector<const TrExtCue*> gone;
    for (const auto& c : d.cues) if (rm.count(c.i)) gone.push_back(&c);
    if (rm.size() != removeIds.size() || gone.size() != rm.size()) { err = L"A subtitle changed meanwhile: nothing was edited."; return false; }
    for (auto& a : adds) {
        if (a.e <= a.s) { err = L"A subtitle must end after it starts."; return false; }   // (0.1 s: the page's rule; an undo may restore less)
        if (a.text.size() > 4000) { err = L"That text is too long."; return false; }
        std::string t;                                     // no control character but a line break
        for (char ch : a.text) { const unsigned char u = static_cast<unsigned char>(ch); if (ch == '\r') continue; if (ch == '\t') { t += ' '; continue; } if (u < 0x20 && ch != '\n') continue; t += ch; }
        a.text = t;
        a.runs = pbsubr::markupRuns(a.text, false);
    }
    // NO NEW OVERLAP (review of WG #5). A subtitle file may hold overlapping lines already -- two voices, a rolling
    // WebVTT -- and every edit of them must stay possible: an edit is refused only when the document would hold MORE
    // overlapping pairs than before (a text fix, a Shift of overlapping lines keep their count). An undo/redo
    // (`restore`) puts back a state the document already had: no check.
    auto over = [](uint64_t s1, uint64_t e1, uint64_t s2, uint64_t e2) { return s1 < e2 && s2 < e1; };
    if (!restore) {
        std::vector<std::pair<uint64_t, uint64_t>> was, now;
        was.reserve(d.cues.size());
        now.reserve(d.cues.size() + adds.size());
        for (const auto& c : d.cues) { was.push_back({ c.s, c.e }); if (!rm.count(c.i)) now.push_back({ c.s, c.e }); }
        for (const auto& a : adds) now.push_back({ a.s, a.e });
        if (pbtrf::overlapPairs(now) > pbtrf::overlapPairs(was)) {
            // name a line in the way: one an added line overlaps (a staying one first)
            uint64_t at = adds.empty() ? 0 : adds.front().s;
            bool found = false;
            for (size_t k = 0; k < adds.size() && !found; ++k) {
                for (const auto& c : d.cues) if (!rm.count(c.i) && over(adds[k].s, adds[k].e, c.s, c.e)) { at = c.s; found = true; break; }
                for (size_t m = 0; m < adds.size() && !found; ++m) if (m != k && over(adds[k].s, adds[k].e, adds[m].s, adds[m].e)) { at = adds[m].s; found = true; }
            }
            err = L"That would overlap the subtitle at " + w16(pbsub::stamp(at, '.')).substr(0, 8) + L": not applied.";
            return false;
        }
    }
    std::vector<TrExtCue> keep;
    keep.reserve(d.cues.size() + adds.size());
    for (auto& c : d.cues) if (!rm.count(c.i)) keep.push_back(std::move(c));
    for (auto& a : adds) { a.i = d.nextI++; added.push_back(a); keep.push_back(std::move(a)); }
    std::stable_sort(keep.begin(), keep.end(), [](const TrExtCue& x, const TrExtCue& y) { return x.s < y.s; });
    d.cues.swap(keep);
    d.dirty = true;
    d.edited = true;
    d.blocked = false;                                       // (a document written nowhere is tried again)
    d.dirtyAt = ::GetTickCount64();
    return true;
}

// Where an edited subtitle file goes, and the write (review of WG #3/#8). The file ITSELF only when writing it back
// from these cues loses nothing (ExtDoc::lossless: a plain SRT or VTT read whole), it is unchanged on disk since it
// was read (or since WE wrote it), and THIS editing session's backup of it is made first (<file>.bak, else .bak-2 ..
// -9: an older backup is never overwritten -- the state before this session stays recoverable). Otherwise the file is
// left untouched and the edits go to a COPY next to it, "<stem>.edited.<srt|vtt>" (an ASS file: .srt), else in
// Documents\SuVtitle Subtitles; later saves go where the first one went. "Save as..." moves them anywhere. A document
// that could be written NOWHERE stays (dirty, blocked until the next edit) and is never dropped without a word.
bool Bridge::saveExtDoc() {
    if (!extDoc_) return true;
    ExtDoc& d = *extDoc_;
    if (!d.dirty || d.blocked) return !d.dirty;
    auto said = [&](bool ok, const std::wstring& name, const std::wstring& bak, const std::wstring& note, const std::wstring& err) {
        JsonW j;
        j.obj().kv(L"type", L"playerSubsSaved").kv(L"id", static_cast<long long>(d.id)).kv(L"ok", ok).kv(L"name", name).kv(L"bak", bak);
        if (!note.empty()) j.kv(L"note", note);
        if (!err.empty()) j.kv(L"err", err);
        j.end();
        postToTranslatorView(j.take());
    };
    const bool sameSource = !d.path.empty() && pbtrf::lowerW(d.path) == pbtrf::lowerW(d.source);
    std::wstring target = d.path, bakName, note;
    // in place (again): the file as it was read, or as we last wrote it -- else someone else changed it
    auto unchanged = [&] { uint64_t s = 0, m = 0; return pbtrf::fileIdentity(d.source, s, m) && s == d.srcSize && m == d.srcMtime; };
    if (target.empty() || sameSource) {
        bool inPlace = d.lossless;
        if (!d.lossless && target.empty())
            note = d.format == pbsubr::Format::Ass
                 ? L"The .ass file keeps styles an .srt cannot hold: it is left as it was."
                 : L"The file holds what SuVtitle does not keep (styles, positions, notes) or lines it could not read: it is left as it was.";
        if (inPlace && !unchanged()) {
            inPlace = false;
            note = L"The file changed on disk since it was opened: it is left as it was.";
        }
        if (inPlace && !d.bakDone) {
            std::wstring bak;
            for (int n = 1; n <= 9 && bak.empty(); ++n) {
                const std::wstring cand = d.source + (n == 1 ? std::wstring(L".bak") : L".bak-" + std::to_wstring(n));
                if (!pbtrf::fileExists(cand)) bak = cand;
            }
            if (bak.empty() || !::CopyFileW(d.source.c_str(), bak.c_str(), TRUE)) {
                inPlace = false;
                note = L"No backup of the file could be made next to it: it is left as it was.";
            } else {
                d.bakDone = true;
                bakName = pbtrf::leafOf(bak);
            }
        }
        target = inPlace ? d.source : std::wstring();
    }
    const std::wstring stem = pbtrf::stripExt(pbtrf::leafOf(d.source));
    const std::wstring ext = (d.format == pbsubr::Format::Vtt) ? L"vtt" : L"srt";
    auto copyIn = [&](const std::wstring& dir) -> std::wstring {
        for (int n = 1; n <= 9; ++n) {
            const std::wstring cand = pbtrf::joinPath(dir, stem + (n == 1 ? L".edited." : L".edited-" + std::to_wstring(n) + L".") + ext);
            if (!pbtrf::fileExists(cand)) return cand;
        }
        return std::wstring();
    };
    std::vector<std::wstring> tries;
    if (!target.empty()) tries.push_back(target);
    else {
        const std::wstring here = copyIn(pbtrf::dirOf(d.source));
        if (!here.empty()) tries.push_back(here);
        // (Documents is a KNOWN folder -- the user's real one even in a test instance's isolated profile: not there)
        const std::wstring docs = pblaunch::isTestInstance() ? std::wstring() : trDocumentsSubtitlesDir();
        if (!docs.empty() && pbtrf::ensureDir(docs)) { const std::wstring there = copyIn(docs); if (!there.empty()) tries.push_back(there); }
    }
    for (const auto& t : tries) {
        const bool vtt = pbtrf::extOf(pbtrf::leafOf(t)) == L"vtt";
        pbtrf::WrittenId wid;
        if (!pbtrf::writeFileAvSafe(t, trExtBytes(d.cues, vtt), true, &wid)) continue;
        if (pbtrf::lowerW(t) == pbtrf::lowerW(d.source)) {     // what we wrote is the file's new "as read"
            uint64_t s = wid.size, m = wid.mtime;
            if (!wid.valid) pbtrf::fileIdentity(t, s, m);
            d.srcSize = s; d.srcMtime = m;
        }
        const bool firstCopy = d.path.empty() || sameSource;
        d.path = t;
        d.dirty = false;
        said(true, pbtrf::leafOf(t), bakName, firstCopy && t != d.source ? note : std::wstring(), L"");
        return true;
    }
    d.blocked = true;                                         // nowhere: kept, said, retried at the next edit
    said(false, L"", L"", L"", L"Your edits could not be saved (no folder accepted the file): use Save as\x2026");
    return false;
}

// The document goes (another subtitle file, the tray's end): written first -- and if it could be written nowhere, its
// loss is SAID, never silent (review of WG #8).
void Bridge::dropExtDoc() {
    if (!extDoc_) return;
    if (extDoc_->dirty) {
        extDoc_->blocked = false;                             // one more try
        if (!saveExtDoc())
            postTranslatorFileNotice(L"Your edits to " + pbtrf::leafOf(extDoc_->source)
                                     + L" could not be saved anywhere and were lost.");
    }
    extDoc_.reset();
}

void Bridge::tickExtDoc() {
    if (!extDoc_ || !player_) return;
    const bool current = player_->loaded() && player_->session().extSubId == extDoc_->id;
    if (extDoc_->dirty && (!current || ::GetTickCount64() - extDoc_->dirtyAt >= 2000)) saveExtDoc();
    // the item moved on: its file is written, the document goes (a document written NOWHERE stays, until the next
    // subtitle file or the tray's end says so -- dropExtDoc)
    if (!current && !extDoc_->dirty) extDoc_.reset();
}

void Bridge::handleSubSaveAs(uint64_t id) {
    if (player_ && player_->isEmbeddedSub(id)) {       // WH: never a dialog that could name the VIDEO as a target
        postTranslatorFileNotice(L"This subtitle track is inside the video file: it is not saved from here.");
        return;
    }
    if (trDialogBusy_ || !extDoc_ || extDoc_->id != id) return;
    const std::wstring src = extDoc_->path.empty() ? extDoc_->source : extDoc_->path;
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Save the edited subtitles");
    const COMDLG_FILTERSPEC f[] = { { L"SubRip subtitles (*.srt)", L"*.srt" }, { L"WebVTT subtitles (*.vtt)", L"*.vtt" } };
    dlg->SetFileTypes(2, f);
    dlg->SetFileTypeIndex(pbtrf::extOf(pbtrf::leafOf(src)) == L"vtt" ? 2 : 1);
    dlg->SetDefaultExtension(L"srt");
    dlg->SetFileName((pbtrf::stripExt(pbtrf::leafOf(src)) + L".edited.srt").c_str());
    {
        IShellItem* item = nullptr;
        const std::wstring dir = pbtrf::dirOf(src);
        if (!dir.empty() && SUCCEEDED(::SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    std::wstring target;
    trDialogBusy_ = true;
    const HRESULT shown = dlg->Show(dialogOwner());
    trDialogBusy_ = false;
    if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) { target = p; ::CoTaskMemFree(p); }
            item->Release();
        }
    }
    dlg->Release();
    if (target.empty() || !extDoc_ || extDoc_->id != id) return;   // (the modal loop may have replaced the document)
    ExtDoc& d = *extDoc_;
    const bool vtt = pbtrf::extOf(pbtrf::leafOf(target)) == L"vtt";
    JsonW j;
    if (pbtrf::writeFileAvSafe(target, trExtBytes(d.cues, vtt), true)) {
        d.path = target;                                    // later edits go there
        d.dirty = d.blocked = false;
        j.obj().kv(L"type", L"playerSubsSaved").kv(L"id", static_cast<long long>(d.id)).kv(L"ok", true)
               .kv(L"name", pbtrf::leafOf(target)).kv(L"bak", L"").end();
    } else {
        j.obj().kv(L"type", L"playerSubsSaved").kv(L"id", static_cast<long long>(d.id)).kv(L"ok", false)
               .kv(L"name", L"").kv(L"bak", L"").kv(L"err", L"The file could not be saved there.").end();
    }
    postToTranslatorView(j.take());
}

// ---- reveal, the automatic .srt, the export ---------------------------------------------------------------------
void Bridge::handleTranslatorFileReveal(const std::wstring& what) {
    std::vector<std::wstring> targets;
    if (what == L"export")      targets.push_back(trLastExport_);
    else if (!player_ || !player_->loaded()) return;
    else if (what == L"media")  targets.push_back(player_->session().path);
    else if (player_->session().set)                 // WG: every language's file -- selected together
        for (const auto& f : player_->session().set->srtFiles) targets.push_back(w16(f.path));
    std::vector<std::wstring> here;                   // the ones on disk, in the FIRST one's folder
    for (const auto& t : targets)
        if (!t.empty() && pbtrf::fileExists(t) && (here.empty() || pbtrf::lowerW(pbtrf::dirOf(t)) == pbtrf::lowerW(pbtrf::dirOf(here.front()))))
            here.push_back(t);
    if (here.empty()) return;
    if (here.size() == 1) {
        PIDLIST_ABSOLUTE pidl = ::ILCreateFromPathW(here.front().c_str());
        if (!pidl) return;
        ::SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ::ILFree(pidl);
        return;
    }
    PIDLIST_ABSOLUTE folder = ::ILCreateFromPathW(pbtrf::dirOf(here.front()).c_str());
    if (!folder) return;
    std::vector<PIDLIST_ABSOLUTE> full;
    std::vector<PCUITEMID_CHILD> kids;
    for (const auto& t : here) {
        PIDLIST_ABSOLUTE p = ::ILCreateFromPathW(t.c_str());
        if (!p) continue;
        full.push_back(p);
        kids.push_back(::ILFindLastID(p));
    }
    if (!kids.empty()) ::SHOpenFolderAndSelectItems(folder, static_cast<UINT>(kids.size()), kids.data(), 0);
    for (auto p : full) ::ILFree(p);
    ::ILFree(folder);
}

// The automatic .srt files (TrFileStore.h holds every naming rule) -- since WG, ONE PER LANGUAGE of the pair,
// "<base>.<tag>.srt" (en, fr, ..., zh-CN, zh-TW) next to the file, so VLC/MPC/Plex load them by themselves (the
// user, 11/10: "un fichier sous-titre par langue, avec le nom du langage dans le nom du fichier"); file L holds each
// cue's text IN L (TrFileStore.h textInLang). "<base>.<ext>.<tag>.srt" when another medium with the same base sits
// there; ".suvtitle.srt", then "-2".."-9", when a file of the usual name is there and is not THIS media's;
// Documents\SuVtitle Subtitles when the file's folder refuses the write. A name that was ABSENT is created without
// replace: a file that appears meanwhile is never clobbered, the next name is tried. WRITTEN AS THE ANALYSIS GOES
// (TrPlayer.h: within 30 s, and on pause / switch / the end), so a file already holding exactly these bytes is left
// alone -- compared, not rewritten (no write-time churn for a player watching it). Returns the path ("" = none) and
// its identity; `note` says anything the user should know about it.
std::wstring Bridge::writeTranslatorSrt(const pbplayer::CueSet& S, const std::string& lang,
                                        const std::vector<pbt::trpipe::WireCue>& cues,
                                        const std::string& fromKey, uint32_t fromOrd,
                                        std::wstring& note, uint64_t& outSize, uint64_t& outMtime) {
    note.clear();
    outSize = outMtime = 0;
    const std::string bytes = pbsub::srt(trLangCues(cues, lang), pbsub::Which::Dst);
    const std::wstring tag  = w16(pbtrf::fileLangTag(lang));
    const std::wstring base = pbtrf::stripExt(S.name);
    const std::wstring ext  = pbtrf::extOf(S.name);
    const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    auto ownsForThisMedia = [&](const std::wstring& p) {
        uint64_t s = 0, m = 0;
        return pbtrf::fileIdentity(p, s, m) && pbtrf::registryOwns(reg, p, s, m, S.path);
    };
    auto exists = [](const std::wstring& p) { return pbtrf::fileExists(p); };
    // (Documents is a KNOWN folder -- the user's real one even in a test instance's isolated profile: not there)
    const std::wstring docs = pblaunch::isTestInstance() ? std::wstring() : trDocumentsSubtitlesDir();
    const std::wstring dirs[2] = { S.folder, docs };
    bool noName = false;
    for (int k = 0; k < 2; ++k) {
        if (dirs[k].empty()) continue;
        if (k == 1 && !pbtrf::ensureDir(dirs[k])) continue;
        const bool sibling = (k == 0) && pbtrf::hasSiblingMedia(trLeavesWithBase(dirs[k], S.name), S.name);
        for (int attempt = 0; attempt < 3; ++attempt) {   // a name taken meanwhile: choose again
            const std::wstring p = pbtrf::chooseSrtPath(dirs[k], base, ext, tag, sibling, exists, ownsForThisMedia);
            if (p.empty()) { noName = true; break; }
            const bool replace = exists(p);               // here only when it is ours for this media
            if (replace) {
                // WG: ours, for this media, and already exactly these bytes: nothing to write
                std::string have;
                uint64_t s0 = 0, m0 = 0;
                if (pbtrf::readFileAll(p, have, bytes.size() + 1) && have == bytes && pbtrf::fileIdentity(p, s0, m0)) {
                    outSize = s0; outMtime = m0;
                    if (k == 1) note = L"The file's folder could not be written to: saved in Documents\\SuVtitle Subtitles.";
                    return p;
                }
            }
            pbtrf::WrittenId id;
            if (!pbtrf::writeFileAvSafe(p, bytes, replace, &id)) {
                if (!replace && exists(p)) continue;      // it appeared under us: the next free name
                break;                                    // this folder refuses the write
            }
            uint64_t s = id.size, m = id.mtime;
            if (!id.valid) pbtrf::fileIdentity(p, s, m);  // the time could not be SET: best effort
            outSize = s; outMtime = m;
            trRegister(p, s, m, S.path, lang, fromKey, fromOrd);   // WH (D6): which record wrote it
            const std::wstring leaf = pbtrf::leafOf(p);
            if (k == 1)
                note = L"The file's folder could not be written to: saved in Documents\\SuVtitle Subtitles.";
            else if (leaf.find(L".suvtitle") != std::wstring::npos)
                note = L"A subtitle file of the usual name was already there: ours is " + leaf + L".";
            else if (sibling)
                note = L"Another recording with the same name is in this folder: ours is " + leaf + L".";
            return p;
        }
    }
    note = noName ? L"The .srt was not saved: every subtitle name for this file is taken by other files."
                  : L"The .srt could not be saved: no folder accepted it.";
    return std::wstring();
}

void Bridge::handleTranslatorFileExport(const std::wstring& format, const std::wstring& which) {
    auto ack = [&](bool ok, const std::wstring& pathOrErr, long long count) {
        JsonW j;
        j.obj();
        j.kv(L"type", L"translatorFileExported");
        j.kv(L"ok", ok);
        j.kv(L"format", format);
        j.kv(L"which", which);
        if (ok) { j.kv(L"path", pathOrErr); j.kv(L"count", count); }
        else    j.kv(L"err", pathOrErr);
        j.end();
        postToTranslatorView(j.take());
    };
    if (trDialogBusy_) return;
    if (format != L"srt" && format != L"vtt" && format != L"docx") {
        ack(false, L"unknown export format", 0);
        return;
    }
    if (!player_ || !player_->loaded() || !player_->session().set || player_->session().set->cues.empty()) {
        ack(false, L"nothing to export yet", 0);
        return;
    }
    // (!) NOTHING OF THE SESSION IS TOUCHED AFTER Show() BELOW (review of WC). The dialog's modal
    // loop dispatches messages: a file opened meanwhile REPLACES the session. Everything needed
    // after the dialog is copied out here -- the cues too, in START order (v2's runs interleave).
    const pbplayer::Session& se = player_->session();
    const pbplayer::CueSet& S = *se.set;
    std::vector<pbt::trpipe::WireCue> cues = S.cues;
    pbtrf::canonicalCues(cues);
    const pbsub::Which w = (which == L"src") ? pbsub::Which::Src
                         : (which == L"both") ? pbsub::Which::Both : pbsub::Which::Dst;
    const std::wstring base = pbtrf::stripExt(S.name);
    const std::wstring tgt = w16(pbsub::langTag(pbtrf::targetLang(S.mode, S.langA, S.langB, cues)));
    const std::wstring spk = w16(pbsub::langTag(pbtrf::spokenLang(cues, S.langA)));
    std::string bytes;
    std::wstring name, ext;
    if (format == L"docx") {
        std::vector<pbdocx::Line> lines;
        lines.reserve(cues.size());
        for (const auto& c : cues) {
            pbdocx::Line l;
            l.mediaMs = (long long)c.startMs;
            l.lang = w16(c.lang); l.dstLang = w16(c.dstLang);
            l.src = w16(c.src);   l.dst = w16(c.dst);
            l.pass = c.pass;
            lines.push_back(std::move(l));
        }
        pbdocx::Doc info;
        info.title = S.name + L" \x2014 SuVtitle";
        info.subtitle = L"Bilingual subtitles"            // U3: its own name -- "Transcript" is the speaker transcript now
                      + (S.totalMs ? L" \x00b7 " + trClock(S.totalMs) : std::wstring())
                      + L" \x00b7 " + std::to_wstring((long long)cues.size()) + L" subtitles \x00b7 "
                      + pbdocx::languageName(w16(S.langA)) + L" \x2194 " + pbdocx::languageName(w16(S.langB))
                      + (S.asr.empty() ? std::wstring() : L" \x00b7 " + S.asr)
                      + (S.mt.empty() ? std::wstring() : L" + " + S.mt)
                      + (S.complete ? std::wstring() : L" \x00b7 partial");
        if (!pbdocx::buildFileTranscript(info, lines, bytes) || bytes.empty()) {
            ack(false, L"the document could not be built", 0);
            return;
        }
        name = base + L" - bilingual subtitles (" + tgt + L").docx";   // (U3: "- transcript" names the speaker transcript)
        ext = L"docx";
    } else {
        const std::vector<pbsub::Cue> cs = trSubCues(cues);
        bytes = (format == L"vtt") ? pbsub::vtt(cs, w) : pbsub::srt(cs, w);
        const std::wstring tag = (w == pbsub::Which::Src) ? spk
                               : (w == pbsub::Which::Both) ? (tgt + L"+" + spk) : tgt;
        name = base + L"." + tag + L"." + format;
        ext = format;
    }
    const long long count = (long long)cues.size();
    const std::wstring mediaPath = S.path;
    const std::wstring startFolder = (!S.folder.empty() && ::GetFileAttributesW(S.folder.c_str()) != INVALID_FILE_ATTRIBUTES)
                                   ? S.folder : trDocumentsDir();
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg))) || !dlg) {
        ack(false, L"the Save dialog could not be opened", 0);
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(format == L"docx" ? L"Save the bilingual subtitles" : L"Save the subtitles");   // (U3: the docx's new name)
    const COMDLG_FILTERSPEC fSrt[]  = { { L"SubRip subtitles (*.srt)", L"*.srt" } };
    const COMDLG_FILTERSPEC fVtt[]  = { { L"WebVTT subtitles (*.vtt)", L"*.vtt" } };
    const COMDLG_FILTERSPEC fDocx[] = { { L"Word document (*.docx)", L"*.docx" } };
    dlg->SetFileTypes(1, format == L"srt" ? fSrt : format == L"vtt" ? fVtt : fDocx);
    dlg->SetFileTypeIndex(1);
    dlg->SetDefaultExtension(ext.c_str());
    dlg->SetFileName(name.c_str());
    {
        // Next to the film (where a player looks for subtitles); Documents when that folder is gone.
        IShellItem* item = nullptr;
        if (!startFolder.empty()
            && SUCCEEDED(::SHCreateItemFromParsingName(startFolder.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    std::wstring target;
    trDialogBusy_ = true;
    const HRESULT shown = dlg->Show(dialogOwner());
    trDialogBusy_ = false;
    if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                target = p;
                ::CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (target.empty()) {
        ack(false, L"cancelled", 0);   // the page says nothing about a deliberate Cancel
        return;
    }
    pbtrf::WrittenId id;   // the user confirmed any overwrite in the dialog: replace is theirs
    if (!pbtrf::writeFileAvSafe(target, bytes, true, &id)) {
        ack(false, L"the file could not be saved there", 0);
        return;
    }
    // A subtitle file we wrote is OURS, for this media: a later automatic .srt may replace it while
    // it is untouched, never once the user has edited it (TrFileStore.h, the registry).
    if (format != L"docx") {
        uint64_t s = id.size, m = id.mtime;
        if (id.valid || pbtrf::fileIdentity(target, s, m)) trRegister(target, s, m, mediaPath);
    }
    trLastExport_ = target;
    ack(true, target, count);
}

// FX (02/11/2026, F4 -- the integrator's MECHANICAL criterion): the sentence for a Word file that refused to be REPLACED although its
// folder took the .tmp (pbtrf::WriteFail::Target) -- held by another program (G2's own sentence), read-only (its own: never "open in
// another program"), or denied. Never "the folder could not be written to", never a copy in Documents: the file stays where it is,
// the tab keeps offering Save .docx..., the next change, Save or Transcribe all tries again.
static std::wstring trDocxRefusal(const std::wstring& path) {
    switch (pbtrf::targetRefusal(path)) {
    case pbtrf::TargetRefusal::Held:     return L"The Word file is open in another program: close it to update it.";
    case pbtrf::TargetRefusal::ReadOnly: return L"The Word file is read-only: it is left as it is. Save .docx\x2026 writes a new one.";
    default:                             return L"The Word file could not be updated.";
    }
}
// FX (F4): a read-only file (its attribute) -- Save .docx... never offers one by default (its sentence promises a new one)
static bool trReadOnlyFile(const std::wstring& path) {
    const DWORD a = ::GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY) != 0;
}

// U3 (02/11/2026, the TRANSCRIPTION): THE TRANSCRIPT'S WORD FILE, written by the CORE's rule (TrPlayer.h writeDocx: at the
// transcript's completion, by Re-transcribe, by "Transcribe all" when missing or older) -- next to the media: "<base> -
// transcript.docx", then "(SuVtitle)", "(SuVtitle 2..9)" ("<base>.<ext> - ..." beside a namesake: TrFileStore.h
// transcriptCandidates); a name is taken only when ABSENT or OURS FOR THIS MEDIA (the registry, kind "transcript": SuVtitle
// wrote it for this media and it is still exactly what was written) -- a file the user edited in Word is never overwritten.
// `t.prefer` (the file written before, still ours) is replaced IN PLACE. Created without replace (a file appearing meanwhile:
// the next name), written AV-safe; a folder that refuses -> Documents\SuVtitle Transcripts (never in a test instance).
// FX (02/11/2026, docs/PROGRESS_FIX.md): F2 -- the user's rule, ONE Word file per media AND sound track: the track's own names
// (transcriptCandidates: "(track N)" for any track but the first) and "ours" = SuVtitle wrote it for this media, this track AND
// this analysis (registryOwnsFor -- an entry with no key owns nothing); when ANOTHER analysis of this track (another pair or
// model) holds the track's Word file, nothing is written by itself (the core asks first, docxState "other": this is the second
// guard, on the disk as it is now). F4 -- the integrator's MECHANICAL criterion: a write whose .tmp the folder TOOK but whose file
// refused to be replaced (pbtrf::WriteFail::Target: held by another program -- Word --, read-only, denied) is said in one sentence
// (trDocxRefusal) and NEVER sent to Documents; Documents only when the folder refused the .tmp itself (WriteFail::Folder).
std::wstring Bridge::writeTranscriptDocx(const pbplayer::CueSet& S, const pbtrs::TranscriptOut& t, std::wstring& note,
                                         uint64_t& outSize, uint64_t& outMtime) {
    note.clear();
    outSize = outMtime = 0;
    std::string bytes;
    if (!trTranscriptBytes(t, bytes)) { note = L"The transcript is empty: no Word file."; return std::wstring(); }
    const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    auto ownsForThisAnalysis = [&](const std::wstring& p) {
        uint64_t s = 0, m = 0;
        return pbtrf::fileIdentity(p, s, m) && pbtrf::registryOwnsFor(reg, p, s, m, S.path, S.key, S.audio.ord);
    };
    auto exists = [](const std::wstring& p) { return pbtrf::fileExists(p); };
    auto finish = [&](const std::wstring& p, const pbtrf::WrittenId& id) {
        uint64_t s = id.size, m = id.mtime;
        if (!id.valid) pbtrf::fileIdentity(p, s, m);      // the time could not be SET: best effort
        outSize = s;
        outMtime = m;
        trRegister(p, s, m, S.path, pbtrf::transcriptRegKind(), S.key, S.audio.ord);
        return p;
    };
    pbtrf::WriteFail fail = pbtrf::WriteFail::None;
    const bool ownPrefer = !t.prefer.empty() && exists(t.prefer) && ownsForThisAnalysis(t.prefer);
    if (ownPrefer) {
        pbtrf::WrittenId id;
        if (pbtrf::writeFileAvSafe(t.prefer, bytes, true, &id, &fail)) return finish(t.prefer, id);
        // F4: the folder took the .tmp, the file refused -- said, kept, retried later; NEVER a folder's refusal, never Documents
        if (fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(t.prefer); return std::wstring(); }
        // (the FOLDER refused the .tmp -- a card write-locked since: the names below, then Documents)
    }
    // F2: ANOTHER analysis of this sound track (another pair or model) holds the track's Word file -> nothing written by itself.
    // FX2 (A5, 03/11/2026, docs/PROGRESS_FX2.md): asked only for an analysis with NO Word file of its own -- one that owns t.prefer
    // and whose folder refused it goes on to the Documents copy, even when another analysis Saved a newer Word file of the track
    // since (registryTrackDocx would name that one: "holds another transcript", wrong for an analysis that has its own)
    if (!ownPrefer) {
        const std::string otherKey = pbtrf::registryTrackDocx(reg, S.path, S.audio.ord, exists);
        if (!otherKey.empty() && otherKey != S.key) {
            note = L"The Word file of this sound track holds another transcript of it (another language pair or model): it is kept.";
            return std::wstring();
        }
    }
    const std::wstring base = pbtrf::stripExt(S.name), ext = pbtrf::extOf(S.name);
    const std::wstring docs = pblaunch::isTestInstance() ? std::wstring() : trDocumentsTranscriptsDir();
    const std::wstring dirs[2] = { S.folder, docs };
    bool noName = false;
    for (int k = 0; k < 2; ++k) {
        if (dirs[k].empty()) continue;
        if (k == 1 && !pbtrf::ensureDir(dirs[k])) continue;
        const bool sibling = (k == 0) && pbtrf::hasSiblingMedia(trLeavesWithBase(dirs[k], S.name), S.name);
        for (int attempt = 0; attempt < 3; ++attempt) {   // a name taken meanwhile: choose again
            const std::wstring p = pbtrf::chooseTranscriptPath(dirs[k], base, ext, sibling, exists, ownsForThisAnalysis, S.audio.ord);
            if (p.empty()) { noName = true; break; }
            const bool replace = exists(p);               // here only when it is ours for this analysis
            pbtrf::WrittenId id;
            if (!pbtrf::writeFileAvSafe(p, bytes, replace, &id, &fail)) {
                if (!replace && exists(p)) continue;      // it appeared under us: the next free name
                if (replace && fail == pbtrf::WriteFail::Target) { note = trDocxRefusal(p); return std::wstring(); }   // F4
                break;                                    // this folder refuses the write
            }
            const std::wstring leaf = pbtrf::leafOf(p);
            if (k == 1)
                note = L"The media's folder could not be written to: the transcript is in Documents\\SuVtitle Transcripts.";
            else if (leaf.find(L"(SuVtitle") != std::wstring::npos)
                note = L"A transcript of the usual name was already there: ours is " + leaf + L".";
            return finish(p, id);
        }
    }
    note = noName ? L"The Word file was not saved: every transcript name for this file is taken by other files."
                  : L"The Word file could not be saved: no folder accepted it.";
    return std::wstring();
}

// U2b (G2, the user's answer 02/11: a rename updates the Word file by itself): a RENAME (or an edit) of a transcript whose Word file
// SuVtitle wrote and nobody changed since -- the core's word (TrPlayer.h docxState "older") -- rewrites THAT file IN PLACE, and
// only while the registry still says it is OURS for this media (an identity it does not know = never touched, nothing said);
// never a new name, never another folder. A write refused because the file is OPEN in another program (Word holds it: a sharing /
// lock violation for WRITE) -> `why` says it in one sentence (the next rename, edit or Save retries); any other refusal -> "The
// Word file could not be updated." Written: registered again with its new identity (it stays ours).
// FX (02/11/2026): F2 -- "ours" = for this media, this sound track AND this analysis (registryOwnsFor: G2 is an automatic write);
// F4 -- the same mechanical criterion and sentences as writeTranscriptDocx (trDocxRefusal: held / read-only / denied).
bool Bridge::rewriteTranscriptDocx(const pbplayer::CueSet& S, const pbtrs::TranscriptOut& t, std::wstring& why,
                                   uint64_t& outSize, uint64_t& outMtime) {
    why.clear();
    outSize = outMtime = 0;
    if (t.prefer.empty()) return false;
    const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
    uint64_t s0 = 0, m0 = 0;
    if (!pbtrf::fileIdentity(t.prefer, s0, m0) || !pbtrf::registryOwnsFor(reg, t.prefer, s0, m0, S.path, S.key, S.audio.ord))
        return false;
    std::string bytes;
    if (!trTranscriptBytes(t, bytes)) return false;
    pbtrf::WrittenId id;
    pbtrf::WriteFail fail = pbtrf::WriteFail::None;
    if (!pbtrf::writeFileAvSafe(t.prefer, bytes, true, &id, &fail)) {         // (the AV-safe write: temp + rename, its retry)
        why = fail == pbtrf::WriteFail::Target ? trDocxRefusal(t.prefer) : std::wstring(L"The Word file could not be updated.");
        return false;
    }
    uint64_t s = id.size, m = id.mtime;
    if (!id.valid) pbtrf::fileIdentity(t.prefer, s, m);   // the time could not be SET: best effort
    outSize = s;
    outMtime = m;
    trRegister(t.prefer, s, m, S.path, pbtrf::transcriptRegKind(), S.key, S.audio.ord);
    return true;
}

// U3: the Transcript tab's "Save .docx..." and the Export menu's "Transcript (.docx)" -- the playing file's transcript AS IT IS
// NOW (a partial one says so in its header), through the Save dialog: by default the Word file written for it while it is
// still ours (SuVtitle wrote it, so it may replace it: Windows' overwrite prompt asks first), else "<base> - transcript.docx"
// next to the media. The file saved is registered as ours for this media; a COMPLETE transcript keeps it as its Word file
// (TrPlayer.h transcriptSaved: "older" is said against it from then on). FIX ROUND R5's hook, tests only: a TEST INSTANCE may
// answer the dialog with SUVTITLE_TEST_EXPORT=<a path> ("cancel" = the dialog's Cancel); the product never reads it.
void Bridge::handleTranscriptSave() {
    auto ack = [&](bool ok, const std::wstring& pathOrErr, long long count) {
        JsonW j;
        j.obj();
        j.kv(L"type", L"translatorFileExported");
        j.kv(L"ok", ok);
        j.kv(L"format", L"transcript");
        j.kv(L"which", L"");
        if (ok) { j.kv(L"path", pathOrErr); j.kv(L"count", count); }
        else    j.kv(L"err", pathOrErr);
        j.end();
        postToTranslatorView(j.take());
    };
    if (trDialogBusy_) return;
    pbtrs::TranscriptOut t;
    if (!player_ || !player_->transcriptForSave(t)) { ack(false, L"nothing to save yet", 0); return; }
    // (!) NOTHING OF THE SESSION IS TOUCHED AFTER Show() (review of WC): the dialog's modal loop dispatches messages, and a
    // file opened meanwhile REPLACES the session -- everything needed after it is copied out here.
    const pbplayer::CueSet& S = *player_->session().set;
    const std::wstring mediaPath = S.path, mediaFolder = S.folder, base = pbtrf::stripExt(S.name);
    const std::string key = S.key;
    const uint32_t ord = S.audio.ord;
    std::string bytes;
    if (!trTranscriptBytes(t, bytes)) { ack(false, L"the document could not be built", 0); return; }
    const long long count = static_cast<long long>(t.turns.size());
    std::wstring startFolder, name;
    // (FX F4: a READ-ONLY Word file of its own is never the default -- its sentence promises a new one)
    if (!t.prefer.empty() && !trReadOnlyFile(t.prefer)) {
        startFolder = pbtrf::dirOf(t.prefer);
        name = pbtrf::leafOf(t.prefer);
    } else {
        startFolder = (!mediaFolder.empty() && ::GetFileAttributesW(mediaFolder.c_str()) != INVALID_FILE_ATTRIBUTES)
                    ? mediaFolder : trDocumentsDir();
        // (FX F2: the sound track's own name -- "(track N)" for any track but the first)
        name = pbtrf::leafOf(pbtrf::transcriptCandidates(startFolder, base, pbtrf::extOf(S.name), false, ord).front());
        // U2b (G2): a Word file of that name the user EDITED since SuVtitle wrote it -- or one that is not SuVtitle's -- is never
        // offered: a NEW name instead, the first transcript name that is free or ours (TrFileStore.h chooseTranscriptPath, the
        // automatic file's own rule; Windows' overwrite prompt would still ask, but the default must not invite it). FX (F2, the
        // user's rule): "ours" = for this sound track AND this analysis -- ANOTHER analysis' Word file of the track is never offered;
        // (F4) nor a read-only one
        if (startFolder == mediaFolder) {
            const std::vector<pbtrf::RegEntry> reg = trLoadRegistry();
            auto owns = [&](const std::wstring& p) {
                uint64_t s = 0, m = 0;
                return pbtrf::fileIdentity(p, s, m) && pbtrf::registryOwnsFor(reg, p, s, m, mediaPath, key, ord) && !trReadOnlyFile(p);
            };
            auto exists = [](const std::wstring& p) { return pbtrf::fileExists(p); };
            const bool sibling = pbtrf::hasSiblingMedia(trLeavesWithBase(mediaFolder, S.name), S.name);
            const std::wstring p = pbtrf::chooseTranscriptPath(mediaFolder, base, pbtrf::extOf(S.name), sibling, exists, owns, ord);
            if (!p.empty()) name = pbtrf::leafOf(p);
        }
    }
    std::wstring target;
    {
        wchar_t tv[MAX_PATH] = {};
        const DWORD tn = pblaunch::isTestInstance() ? ::GetEnvironmentVariableW(L"SUVTITLE_TEST_EXPORT", tv, MAX_PATH) : 0;
        if (tn > 0 && tn < MAX_PATH) {
            if (std::wstring(tv) == L"cancel") { ack(false, L"cancelled", 0); return; }
            target = tv;
        }
    }
    if (target.empty()) {
        IFileSaveDialog* dlg = nullptr;
        if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) || !dlg) {
            ack(false, L"the Save dialog could not be opened", 0);
            return;
        }
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Save the transcript");
        const COMDLG_FILTERSPEC fDocx[] = { { L"Word document (*.docx)", L"*.docx" } };
        dlg->SetFileTypes(1, fDocx);
        dlg->SetFileTypeIndex(1);
        dlg->SetDefaultExtension(L"docx");
        dlg->SetFileName(name.c_str());
        IShellItem* folder = nullptr;
        if (!startFolder.empty()
            && SUCCEEDED(::SHCreateItemFromParsingName(startFolder.c_str(), nullptr, IID_PPV_ARGS(&folder))) && folder) {
            dlg->SetFolder(folder);
            folder->Release();
        }
        trDialogBusy_ = true;
        const HRESULT shown = dlg->Show(dialogOwner());
        trDialogBusy_ = false;
        if (SUCCEEDED(shown)) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR p = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
                    target = p;
                    ::CoTaskMemFree(p);
                }
                item->Release();
            }
        }
        dlg->Release();
        if (target.empty()) { ack(false, L"cancelled", 0); return; }
    }
    pbtrf::WrittenId id;                                  // the user confirmed any overwrite in the dialog: replace is theirs
    if (!pbtrf::writeFileAvSafe(target, bytes, true, &id)) { ack(false, L"the file could not be saved there", 0); return; }
    uint64_t s = id.size, m = id.mtime;
    if (id.valid || pbtrf::fileIdentity(target, s, m)) {
        trRegister(target, s, m, mediaPath, pbtrf::transcriptRegKind(), key, ord);
        // the same analysis still on screen (the dialog may have let another file in): it keeps this file as its Word file
        if (player_ && player_->loaded() && player_->session().set && player_->session().set->key == key)
            player_->transcriptSaved(target, s, m, t.sig, t.tc);   // (FX F1: with the timecode its stamps were made with)
    }
    trLastExport_ = target;
    ack(true, target, count);
}

// ============================================================================
// the 1 Hz tick (main.cpp's WM_TIMER)
// ============================================================================
void Bridge::uiTick() {
    if (!running_.load()) return;
    // THE SAFETY NET, and deliberately the full pump rather than the bare drain: it also clears a pending flag whose
    // post never arrived, so one lost message can never wedge the wake path for the rest of the session (W1).
    if (translator_) pumpTranslator();
    // WE: the player's clock -- checkpoints of a running job's partial result, the queue and the
    // positions written, the pre-analysis scheduled, the edited subtitle file's 2 s save.
    if (player_) { drainPlayerScans(); drainPlayerTracks(); player_->tick(); tickExtDoc(); }
    // 1.1.0 NATIVE: the safety net of the sync, and a state the rate limit held back
    syncNative();
    if (nativeDirty_) postNativeState(true, false);
    // SUVTITLE LIVE fix round (RVB-13): a trim's LIVEIN, once it has rested 1 s
    if (liveCfgSaveDue_ && ::GetTickCount64() >= liveCfgSaveDue_) flushLiveConfig();
}

// ============================================================================
// THE NATIVE VIDEO ENGINE (1.1.0, 2026-10-26, agent N2 -- docs/NATIVE_CONTRACT.md sections 2-4 and 6, NATIVE_ENGINE_PLAN.md).
//
// The user: "ajouter un vrai moteur video natif" -- "que SuVtitle puisse etre installe sur n'importe quel ordinateur et
// puisse lire le plus de formats possible sans s'appuyer sur une carte graphique". mpv (MpvPlayer.h, N1's) plays EVERY file
// in a child window under the page; the page draws the interface over it. This section is the bridge between the two:
//   * the core's CURRENT item is opened in mpv HERE (syncNative, after every entry point that can open or close one): the
//     path from the core's session, the start from its resume position, the sound track from its pick -- never a path from
//     the page (D1). mpv opens it PAUSED: the page plays it (play when ready, keep ahead, a hidden window...);
//   * the page's nativeCmd -> MpvPlayer calls, for the current opening only (gen = Session::seq; a stale gen is dropped);
//     its nativeRect -> placeCss (CSS px; MpvPlayer owns the DPI maths); its nativeCursor -> main.cpp (wParam 5);
//   * mpv's events -> nativeState (built from MpvPlayer::state(): <= 40 Hz while it plays, at once on a discrete change),
//     nativeError (mpv refused this file: the page plays it with its <video>), nativeInfo (usable or not, and why);
//   * the SOUND COPY (FPROXY) is the <video>'s only: the core asks none while mpv plays the item (PlayerHost::mpvPlays), and
//     asks it when the item falls back with no URL (mpvDropped) -- fix round, L2 / L3;
//   * mpv's sound-track list goes to the core for the containers MediaTracks does not read (offerNativeTracks: L6).
// UI THREAD ONLY (MpvPlayer delivers its events on the window's thread).
// ============================================================================
namespace {
// a number member of a JsonR object (the page's nativeRect / nativeCmd): finite and inside [lo, hi], else false
bool nativeNum(const pbjson::Value* x, double lo, double hi, double& out) {
    if (!x || x->kind != pbjson::Value::Kind::Num || !std::isfinite(x->n) || x->n < lo || x->n > hi) return false;
    out = x->n;
    return true;
}
unsigned long long steadyMs() {
    return static_cast<unsigned long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
constexpr unsigned long long kNativeStateEveryMs = 25;   // MpvPlayer coalesces Time to <= 30 Hz already: a guard, not a pace
// fix round (L6): a string of the FILE's that mpv listed (a sound track's title, language, codec) -- untrusted, whatever
// MpvPlayer did with it already: cut at a NUL, control characters dropped, at most `cap` bytes (MediaTracks' own caps:
// 4096 for a name, 64 for a language), cut on a UTF-8 boundary. The JSON writers escape what is left.
std::string nativeText(const std::string& s, size_t cap) {
    std::string o;
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == 0 || o.size() >= cap) break;
        if (c < 0x20 || c == 0x7f) continue;
        o += ch;
    }
    size_t i = o.size();                          // the last character complete, or dropped
    while (i > 0 && (static_cast<unsigned char>(o[i - 1]) & 0xC0) == 0x80) --i;
    if (i > 0) {
        const unsigned char lead = static_cast<unsigned char>(o[i - 1]);
        const size_t want = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
        if (o.size() - (i - 1) < want) o.resize(i - 1);
    }
    return o;
}
// mpv names a codec as FFmpeg does ("ac3", "pcm_s16le"): the Audio list says what MediaTracks says for the same codec
// ("AC-3", "PCM") -- webViewPlays() and the menu read the same words whatever listed the track
std::string nativeCodecName(const std::string& c) {
    static const struct { const char* ff; const char* name; } kNames[] = {
        { "aac", "AAC" }, { "aac_latm", "AAC" }, { "mp3", "MP3" }, { "mp3float", "MP3" }, { "mp2", "MP2" }, { "mp2float", "MP2" },
        { "mp1", "MP1" }, { "ac3", "AC-3" }, { "eac3", "E-AC-3" }, { "dts", "DTS" }, { "truehd", "TrueHD" }, { "mlp", "MLP" },
        { "opus", "Opus" }, { "vorbis", "Vorbis" }, { "flac", "FLAC" }, { "alac", "ALAC" }, { "wmav1", "WMA" }, { "wmav2", "WMA" },
        { "wmapro", "WMA" }, { "wmalossless", "WMA" }, { "wmavoice", "WMA" }, { "amr_nb", "AMR" }, { "amrnb", "AMR" },
        { "amr_wb", "AMR-WB" }, { "amrwb", "AMR-WB" }, { "pcm_mulaw", "G.711" }, { "pcm_alaw", "G.711" }, { "tta", "TTA" },
        { "wavpack", "WavPack" }, { "cook", "RealAudio" }, { "ra_144", "RealAudio" }, { "ra_288", "RealAudio" } };
    for (const auto& n : kNames) if (c == n.ff) return n.name;
    if (c.compare(0, 4, "pcm_") == 0) return c.size() > 2 && c.compare(c.size() - 2, 2, "be") == 0 ? "PCM (big-endian)" : "PCM";
    if (c.compare(0, 6, "adpcm_") == 0) return "ADPCM";
    return c;
}
}   // namespace

void Bridge::attachNative(suvmpv::MpvPlayer* player, const std::string& why) {
    if (native_ && native_ != player) { native_->setEventSink(nullptr); if (nativeSeq_) native_->stop(); }
    if (native_ != player) { nativeSeq_ = 0; resetNativeItem(); nativeLastJson_.clear(); }
    native_ = player;
    nativeKnown_ = true;
    nativeWhy_ = why;
    if (native_) native_->setEventSink([this](const suvmpv::MpvEvent& e) { onNativeEvent(e); });
    postNativeInfo(false);
    syncNative();   // an item the core opened before the engine was here (--play at the start) plays in it now
}

void Bridge::detachNative() {
    if (!native_) return;
    native_->setEventSink(nullptr);
    if (nativeSeq_) native_->stop();
    native_ = nullptr;
    nativeSeq_ = 0;
    resetNativeItem();
}

// What the bridge knows of the opening mpv holds (none now, or a new one): told again by its events.
void Bridge::resetNativeItem() {
    nativeStarted_ = nativeLoaded_ = nativeTracksSeen_ = nativeRestarted_ = nativeVideoKnown_ = false;
    nativeTracksSig_.clear();
}

bool Bridge::nativeUsable() const { return native_ != nullptr && native_->available(); }

bool Bridge::nativeWanted() const {
    if (!nativeUsable() || !player_ || !player_->loaded()) return false;
    const uint64_t seq = player_->session().seq;
    return seq != 0 && seq != nativeFellBack_;
}

void Bridge::postNativeInfo(bool force) {
    if (!nativeKnown_ && !force) return;
    const bool avail = nativeUsable();
    if (!force && nativeInfoSent_ && avail == nativeInfoAvail_) return;
    std::string why;
    if (!avail) {
        why = native_ ? native_->why() : nativeWhy_;
        if (why.empty()) why = nativeKnown_ ? "the native video engine could not start" : "the native video engine is not ready yet";
    }
    nativeInfoSent_ = true;
    nativeInfoAvail_ = avail;
    if (!webview_) return;
    JsonW j;
    j.obj().kv(L"type", L"nativeInfo").kv(L"available", avail).kv(L"why", w16(why)).end();
    postToTranslatorView(j.take());
}

// A page that (re)loaded: the engine, what mpv holds (adopted, not reopened) -- or, for an item the page's <video> plays
// already (a fallback the old page knew of), that refusal again: a new page must not wait 20 s for a state that will not come.
void Bridge::replayNative() {
    postNativeInfo(true);
    postNativeState(true, true);
    if (nativeFellBack_ && player_ && player_->loaded() && player_->session().seq == nativeFellBack_ && webview_) {
        JsonW j;
        j.obj().kv(L"type", L"nativeError").kv(L"gen", static_cast<long long>(nativeFellBack_)).kv(L"text", L"").end();
        postToTranslatorView(j.take());
    }
}

// The core's current item, and mpv: a NEW opening is opened (paused, at its start, on its sound track), none = mpv stops.
// An opening the page's <video> plays already (a fallback) is left to it. Cheap: called after every entry point.
void Bridge::syncNative() {
    if (!player_) return;
    const uint64_t seq = player_->loaded() ? player_->session().seq : 0;
    if (!seq) {
        if (nativeSeq_) {
            if (native_) native_->stop();
            nativeSeq_ = 0;
            resetNativeItem();
        }
        return;
    }
    if (seq == nativeSeq_ || seq == nativeFellBack_) return;
    if (!nativeUsable()) {
        // (not handed over YET -- the window still being built -- is not "unusable": it opens when attachNative comes)
        if (!nativeKnown_) return;
        nativeSeq_ = 0;
        nativeFellBack_ = seq;
        // ALWAYS said: a page that was told the engine is there falls back NOW (not after its 20 s wait), and one that knew
        // it was not keeps this item on its <video> even if the engine comes back meanwhile (the bridge will not open it)
        JsonW j;
        j.obj().kv(L"type", L"nativeError").kv(L"gen", static_cast<long long>(seq))
               .kv(L"text", w16(native_ ? native_->why() : nativeWhy_)).end();
        postToTranslatorView(j.take());
        postNativeInfo(false);
        // fix round (L3): the <video>'s item now -- with no URL its sound copy is asked HERE (the core asked none while mpv
        // was to play it: never an item left on "Preparing its sound..."), and a job held for mpv's track list starts
        player_->mpvDropped(seq);
        return;
    }
    const pbplayer::Session& s = player_->session();
    nativeSeq_ = seq;
    resetNativeItem();
    nativeStartMs_ = s.startMs;
    native_->open(s.path, s.startMs, s.audioOrd, seq);   // PAUSED, its A-B loop cleared (MpvPlayer.h)
    postNativeState(true, true);                          // the page knows at once which opening is coming, and where
}

// mpv refused this opening (or the page gave up waiting): it is the page's <video>'s now -- mpv lets go of it, the page
// is told (when the refusal is mpv's), and the core is told too (fix round, L2): an item with no URL asks for its sound
// copy now (the copy is the <video>'s: none was asked while mpv was to play it), a job held for mpv's track list starts.
void Bridge::nativeFallBack(uint64_t seq, const std::wstring& text, bool tellPage) {
    if (!seq) return;
    if (seq == nativeSeq_) {
        if (native_) native_->stop();
        nativeSeq_ = 0;
        resetNativeItem();
    }
    nativeFellBack_ = seq;
    if (tellPage) {
        JsonW j;
        j.obj().kv(L"type", L"nativeError").kv(L"gen", static_cast<long long>(seq)).kv(L"text", text).end();
        postToTranslatorView(j.take());
    }
    if (player_) player_->mpvDropped(seq);
}

// fix round (L6, the DECIDED feature): mpv's sound tracks, offered to the core for the CURRENT opening -- it adopts them
// where MediaTracks has no list (AVI, TS / M2TS, MPG / VOB, WMV, FLV, OGG..., or a probe that failed): the right-click menu,
// the CC panel and setAudio then work as for a Matroska file, and the analysis takes the same ord. Offered at Tracks /
// Loaded / Track (the track heard) / the first Restart (mpv's choice is final), each time something changed (the
// signature). The strings are the FILE's: nativeText (cut, capped, whole characters); `track` = mpv's id.
void Bridge::offerNativeTracks() {
    if (!player_ || !native_ || !nativeSeq_ || !player_->loaded() || player_->session().seq != nativeSeq_) return;
    const suvmpv::MpvState& s = native_->state();
    if (s.gen != nativeSeq_) return;
    auto info = std::make_shared<pbmedia::Info>();
    info->state = pbmedia::Info::State::Ready;
    info->container = "mpv";                       // (the core: a list mpv gave -- never a MediaTracks one to keep)
    info->hasVideo = s.hasVideo;
    const char kSep = '\x1f';                      // (nativeText drops control characters: the signature cannot be forged)
    std::string sig = std::to_string(nativeSeq_) + kSep + std::to_string(s.ord) + kSep + (nativeRestarted_ ? "f" : "-");
    for (const suvmpv::MpvState::Audio& a : s.audioList) {
        if (info->audio.size() >= pbmedia::kMaxTracks) break;
        pbmedia::AudioTrack t;
        t.ord = static_cast<uint32_t>(info->audio.size());
        t.track = a.id > 0 && a.id <= 0xFFFFFFFFLL ? static_cast<uint32_t>(a.id) : 0;
        t.name = nativeText(a.title, 4096);
        t.lang = nativeText(a.lang, 64);
        t.codec = nativeCodecName(nativeText(a.codec, 64));
        t.ch = static_cast<uint32_t>(std::min(255, std::max(0, a.channels)));
        t.def = a.isDefault;
        t.play = true;                             // mpv plays every track it lists (the <video> never gets these)
        sig += kSep + std::to_string(t.track) + kSep + t.lang + kSep + t.name + kSep + t.codec + kSep + std::to_string(t.ch)
             + (t.def ? "d" : "");
        info->audio.push_back(std::move(t));
    }
    if (sig == nativeTracksSig_) return;
    nativeTracksSig_ = sig;
    const int ord = s.ord >= 0 && static_cast<size_t>(s.ord) < info->audio.size() ? s.ord : -1;
    player_->offerMpvTracks(nativeSeq_, std::move(info), ord, nativeRestarted_);
}

void Bridge::onNativeEvent(const suvmpv::MpvEvent& e) {
    using K = suvmpv::MpvEvent::Kind;
    if (e.kind == K::Available) {                        // GLOBAL: the engine became usable / unusable
        if (!e.flag && nativeSeq_) nativeFallBack(nativeSeq_, w16(e.text), true);
        postNativeInfo(false);
        syncNative();
        return;
    }
    if (!nativeSeq_ || e.gen != nativeSeq_) return;      // another opening's (mpv still on the previous one): dropped
    switch (e.kind) {
        case K::Error:
            nativeFallBack(nativeSeq_, w16(e.text.empty() ? std::string("mpv could not play this file") : e.text), true);
            return;
        // mpv began loading it (start-file): the page's 20 s wait starts again from here (L7) -- a sleeping NAS or a slow share
        // may have held mpv's I/O until now
        case K::Start:    resetNativeItem(); nativeStarted_ = true; break;
        // its track list (BEFORE Loaded): a picture or not is hasVideo (L9: known at once, cover art excluded -- an mp3 with
        // a cover stays a sound card), never the first frame's size; and mpv's sound tracks go to the core (L6)
        case K::Tracks:
            nativeTracksSeen_ = nativeVideoKnown_ = true;
            offerNativeTracks();
            break;
        case K::Loaded:   nativeLoaded_ = true; offerNativeTracks(); break;
        case K::Track:    offerNativeTracks(); break;
        case K::Duration: case K::Time: nativeLoaded_ = true; break;
        case K::VideoSize:
            nativeLoaded_ = true;
            if (!nativeTracksSeen_ && e.w > 0 && e.h > 0) nativeVideoKnown_ = true;
            break;
        // the first frame is out (or the sound started). A host with no track list: what the size says NOW is the answer
        // (0 x 0 = no picture). mpv's choice of sound track is final from here: the core stops waiting for it
        case K::Restart:
            nativeLoaded_ = true;
            if (!nativeTracksSeen_) nativeVideoKnown_ = true;
            if (!nativeRestarted_) { nativeRestarted_ = true; offerNativeTracks(); }
            break;
        default: break;
    }
    postNativeState(e.kind != K::Time, false);
}

void Bridge::postNativeState(bool now, bool force) {
    if (!webview_ || !native_ || !nativeSeq_) { nativeDirty_ = false; return; }
    const unsigned long long t = steadyMs();
    if (!now && !force && t - nativePostedAt_ < kNativeStateEveryMs) { nativeDirty_ = true; return; }
    const suvmpv::MpvState& s = native_->state();
    const bool mine = s.gen == nativeSeq_;               // (before mpv's start-file of it: the opening as asked)
    JsonW j;
    j.obj();
    j.kv(L"type", L"nativeState");
    j.kv(L"gen", static_cast<long long>(nativeSeq_));
    j.kv(L"timeMs", static_cast<long long>(mine ? s.timeMs : static_cast<long long>(nativeStartMs_)));
    j.kv(L"durMs", static_cast<long long>(mine ? s.durMs : -1));
    j.kv(L"paused", mine ? s.paused : true);
    j.kv(L"eof", mine && s.eof);
    j.kv(L"seeking", mine && s.seeking);
    j.kv(L"volPct", static_cast<long long>(s.volPct));
    j.kv(L"muted", s.muted);
    j.kv(L"ord", static_cast<long long>(mine ? s.ord : -1));
    j.kv(L"tracks", static_cast<long long>(mine ? s.audioTracks : 0));
    j.kv(L"abA", static_cast<long long>(mine ? s.abA : -1));
    j.kv(L"abB", static_cast<long long>(mine ? s.abB : -1));
    j.kv(L"w", static_cast<long long>(mine ? s.w : 0));
    j.kv(L"h", static_cast<long long>(mine ? s.h : 0));
    j.kv(L"hwdec", w16(mine ? s.hwdec : std::string()));
    j.kv(L"warp", s.warp);
    j.kv(L"seekable", mine && s.seekable);
    j.kv(L"loaded", mine && nativeLoaded_);
    j.kv(L"started", mine && nativeStarted_);       // fix round (L7): mpv began loading THIS opening (the page's wait restarts)
    j.kv(L"restarted", mine && nativeRestarted_);   // fix round (M2): its first frame / sound is out -- `seekable` is mpv's word
    // L9: from the track list when the host gave one (hasVideo: a picture track that is not cover art), else the first
    // frame's size
    if (mine && nativeVideoKnown_) j.kv(L"video", nativeTracksSeen_ ? s.hasVideo : s.w > 0 && s.h > 0);
    j.end();
    std::wstring json = j.take();
    // the speed is a fraction (1.25): JsonW writes integers only -- spliced before the closing brace (TrPlayer.h's tracks
    // the same way), in thousandths, with no locale in the way
    const double speed = std::isfinite(s.speed) && s.speed > 0 ? std::min(100.0, s.speed) : 1.0;   // (never NaN on the wire)
    const long long pm = std::llround(speed * 1000.0);
    wchar_t sp[48];
    std::swprintf(sp, 48, L",\"speed\":%lld.%03lld}", pm / 1000, pm % 1000);
    if (!json.empty() && json.back() == L'}') { json.pop_back(); json += sp; }
    nativeDirty_ = false;
    if (!force && json == nativeLastJson_) return;
    nativeLastJson_ = json;
    nativePostedAt_ = t;
    postToTranslatorView(json);
}

// The page's three native messages. Numbers and tokens only (JsonR: a quote or a backslash cannot fool it); every key read
// is on D1's closed list (test_host_pins.js) -- none names a path.
void Bridge::handleNativeMessage(const std::wstring& type, const std::wstring& json) {
    pbjson::Value v;
    if (!pbjson::parse(json, v, 1u << 16) || !v.isObj()) return;
    if (type == L"nativeCursor") {
        // the page left (show) / entered full screen's idle state: main.cpp puts the cursor right (N1's safety net)
        const HWND h = hostWnd_.load();
        if (h) ::PostMessageW(h, kWmTranslatorWindow, kWinNativeCursor, v.flag(L"show", true) ? 1 : 0);
        return;
    }
    if (!native_) return;
    if (type == L"nativeRect") {
        double x = 0, y = 0, w = 0, hh = 0;
        if (!nativeNum(v.get(L"x"), -1e5, 1e5, x) || !nativeNum(v.get(L"y"), -1e5, 1e5, y)
            || !nativeNum(v.get(L"w"), 0, 1e5, w) || !nativeNum(v.get(L"h"), 0, 1e5, hh)) return;
        native_->placeCss(x, y, w, hh);
        return;
    }
    // nativeCmd: the CURRENT opening only
    uint64_t gen = 0;
    if (!v.u64(L"gen", gen) || gen == 0 || gen != nativeSeq_) return;
    const std::wstring cmd = v.str(L"cmd");
    double d = 0;
    uint64_t n = 0;
    if (cmd == L"play") native_->play();
    else if (cmd == L"pause") native_->pause();
    else if (cmd == L"seek") { if (v.u64(L"ms", n)) native_->seek(n, v.flag(L"exact", true)); }
    else if (cmd == L"speed") { if (nativeNum(v.get(L"rate"), 0.25, 4.0, d)) native_->setSpeed(d); }
    else if (cmd == L"volume") { if (v.u64(L"pct", n) && n <= 100) native_->setVolume(static_cast<int>(n)); }
    else if (cmd == L"mute") native_->setMute(v.flag(L"on"));
    else if (cmd == L"ab") {
        // null (or absent) = off; B only with A, after it (MpvPlayer: setting B jumps to A)
        double a = -1, b = -1;
        if (!nativeNum(v.get(L"aMs"), 0, 1e12, a)) a = -1;
        if (a < 0 || !nativeNum(v.get(L"bMs"), 0, 1e12, b) || b <= a) b = -1;
        native_->setAbLoop(a < 0 ? -1 : static_cast<int64_t>(std::llround(a)), b < 0 ? -1 : static_cast<int64_t>(std::llround(b)));
    }
    else if (cmd == L"audio") { if (v.u64(L"ord", n) && n < pbmedia::kMaxTracks) native_->selectAudio(static_cast<int>(n)); }
    else if (cmd == L"frame") { if (nativeNum(v.get(L"step"), -1, 1, d) && (d == 1 || d == -1)) native_->frameStep(static_cast<int>(d)); }
    else if (cmd == L"fallback") nativeFallBack(gen, std::wstring(), false);   // (the page plays it already: nothing to tell)
}

// ============================================================================
// SUVTITLE LIVE (1.1.0, 2026-10-25) -- the LIVE translator, in its own window.
//
// PORTED from AudioPB Translator 1.1.0 (PatchBus commit 50f79d6, ui/bridge.cpp: attach/detachTranslatorView, the
// translator window's messages, translatorSpeakerColor, sendTranslatorState's spk + sessionLines, replayTranslatorLog,
// appendTranslatorSession, trJsonString, postTranslatorExported, handleTranslatorExport -- pollTranslatorOnce's LINE /
// PART / END loop is above, in place) with the decisions of docs/LIVE_PLAN.md:
//   * the Live view has its OWN handler: what it says is about the LIVE scope -- its pair (CTL scope=live), its Clear,
//     its strips, its Listen. 1.1.0 needed a trFromWindow_ flag around one shared handler; here the view is the scope;
//   * a speaker is a STRIP of the capture (named by the user), not a card of AudioPB's desk -- stripLabelFor and the
//     label rule in pollTranslatorOnce;
//   * new, the contract's own: liveConfig (the strips, checked by the engine's rules, persisted as app.txt LIVEIN),
//     liveListen (LIVE 1 / LIVE 0, the player's gate), liveSources (the picker's list), liveLevels (the meters);
//   * the journal is %APPDATA%\SuVtitle\Live sessions\, the document "SuVtitle Live -- <date>".
// UI THREAD ONLY, like everything of the Bridge.
// ============================================================================

// The FIRST view's attach starts the engine (LIVE_PLAN 4.3): the player's normally -- main.cpp opens the Live window once
// the player's view exists -- or the Live window's, should it ever come first. Once: the supervisor respawns it.
void Bridge::startEngine() {
    if (engineStarted_) return;
    engineStarted_ = true;
    if (translator_) {
        if (pblaunch::isTestInstance()) translator_->setSpawnArgs(L"--cpu");
        translator_->start();
    }
    running_.store(true);
    if (player_) player_->setHelperAvailable(translator_ != nullptr);
}

// The Live window's view joins: its own handler (it captures `this`, hence the token: it is unsubscribed BEFORE the
// view goes -- detachLiveView, shutdown), its HWND for the dialogs it asks for.
void Bridge::attachLiveView(ICoreWebView2* webview, HWND hwnd) {
    if (liveView_ == webview) { liveHwnd_ = hwnd; return; }
    detachLiveView();
    liveView_ = webview;
    liveHwnd_ = hwnd;
    livePageReady_ = false;
    if (!liveView_) return;
    startEngine();
    EventRegistrationToken token{};
    liveView_->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                LPWSTR raw = nullptr;
                if (FAILED(args->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
                std::wstring json(raw);
                ::CoTaskMemFree(raw);
                handleLiveMessage(json);
                return S_OK;
            }).Get(),
        &token);
    liveViewToken_ = token.value;
}

void Bridge::detachLiveView() {
    if (liveView_ && liveViewToken_) {
        EventRegistrationToken t; t.value = liveViewToken_;
        liveView_->remove_WebMessageReceived(t);
    }
    liveView_ = nullptr;
    liveViewToken_ = 0;
    liveHwnd_ = nullptr;
    livePageReady_ = false;
    lastLiveStateJson_.clear();
}

// The player's view is gone while the app lives on (the browser process died: main.cpp rebuilds the views and attaches
// again -- attach() takes a view once it has none). The handler first (it captures `this`), then what died with it.
void Bridge::detachView() {
    if (webview_ && webMsgToken_) {
        EventRegistrationToken t; t.value = webMsgToken_;
        webview_->remove_WebMessageReceived(t);
        webMsgToken_ = 0;
    }
    webview_ = nullptr;
    trHwnd_ = nullptr;
    trMappedHosts_.clear();
    trWin_.detached();
    lastTranslatorStateJson_.clear();
    if (player_) { player_->windowDetached(); player_->windowShown(false); }
}

void Bridge::liveTopmostChanged(bool on) {
    liveTopmost_ = on;
    postLiveWindowState();   // (a page still loading gets it on its `ready`)
}

// The Live window closed (hidden): listening stops -- in AudioPB the CARD was the switch and the window only a view; here
// the window IS the switch (the study section 3.1). Nothing else ends: the session, its journal, the strips stay.
void Bridge::liveWindowClosed() {
    if (liveWanted_) handleLiveListen(false);
    if (translator_) translator_->stopSources();   // (fix round, RVB-7: its picker went with it)
}

void Bridge::playerWindowShown(bool on) {
    if (trWin_.shown == on) return;
    trWin_.shown = on;
    postTranslatorWindowState();    // hidden: the page pauses what it plays (applyWindow, explicit, never inferred)
    if (player_) player_->windowShown(on);
}

void Bridge::postToLive(const std::wstring& json) {
    if (liveView_) liveView_->PostWebMessageAsJson(json.c_str());
}

void Bridge::postLiveWarn(const std::wstring& text) {
    JsonW j;
    j.obj().kv(L"type", L"warn").kv(L"text", text).end();
    postToLive(j.take());
}

// RV4-7 for the Live window: its REAL always-on-top, to a page that said `ready` (its `ready` gets it anyway).
void Bridge::postLiveWindowState() {
    if (!liveView_ || !livePageReady_) return;
    JsonW j;
    j.obj().kv(L"type", L"translatorWindow").kv(L"topmost", liveTopmost_).end();
    postToLive(j.take());
}

// ---- the Live view's messages (the contract: LIVE_PLAN 3.3) ---------------------------------------------------------
// Every field read here is a closed-vocabulary token, a number, or a key the ENGINE listed -- no path, no free text
// reaches anything but a strip's LABEL (display only: never a key, never a file name).
void Bridge::handleLiveMessage(const std::wstring& json) {
    std::wstring type;
    if (!jsonFindString(json, L"type", type)) return;
    if (type == L"ready" || type == L"translatorReplay") {
        // The Live page (re)loaded -- or asks again: its state (the LIVE pair), the scrollback (the last 200 finished
        // lines, verbatim), the picker's list if one is known, the window's REAL pin -- the contract's order.
        // (fix round, RVB-7: a page that (re)loaded has its picker CLOSED -- no engine is asked for lists any more)
        if (type == L"ready" && translator_) translator_->stopSources();
        livePageReady_ = true;
        sendLiveState(/*force=*/true);
        replayTranslatorLog();
        sendLiveSources();
        postLiveWindowState();
        return;
    }
    if (type == L"translatorCtl") {
        // Only what changed travels (1.1.0's contract): absent fields are read as "" and leave their setting alone.
        std::wstring mode, preset, langA, langB;
        jsonFindString(json, L"mode", mode);
        jsonFindString(json, L"preset", preset);
        jsonFindString(json, L"langA", langA);
        jsonFindString(json, L"langB", langB);
        bool clear = false;
        if (json.find(L"\"clear\"") != std::wstring::npos) jsonFindBool(json, L"clear", clear);
        // the pin = the LIVE window's always-on-top (main.cpp decides, and tells the page what Windows made of it)
        bool topOn = false;
        if (json.find(L"\"alwaysOnTop\"") != std::wstring::npos && jsonFindBool(json, L"alwaysOnTop", topOn) && hostWnd_)
            ::PostMessageW(hostWnd_, kWmTranslatorWindow, kWinLiveTopmost, topOn ? 1 : 0);
        handleLiveCtl(mode, preset, clear, langA, langB);
        return;
    }
    if (type == L"translatorExport") {
        std::wstring fmt;
        jsonFindString(json, L"format", fmt);
        exportOwner_ = liveHwnd_;            // the Save dialog belongs to the window that asked (1.1.0's trFromWindow_)
        handleTranslatorExport(fmt);
        exportOwner_ = nullptr;
        return;
    }
    if (type == L"liveSources") {            // the picker opens: a fresh list (the engine pushes changes from then on)
        // Fix round (RVB-7): liveSources{open:false} = the picker CLOSED -- the ask ends there (a respawned engine is
        // not asked any more, and nothing enumerates for a list nobody reads).
        bool open = true;
        if (json.find(L"\"open\"") != std::wstring::npos) jsonFindBool(json, L"open", open);
        if (!open) {
            if (translator_) translator_->stopSources();
            return;
        }
        if (translator_) translator_->requestSources();
        sendLiveSources();                   // ...and the one known meanwhile, at once
        return;
    }
    if (type == L"liveConfig") { handleLiveConfig(json); return; }
    if (type == L"liveListen") {
        bool on = false;
        jsonFindBool(json, L"on", on);
        handleLiveListen(on);
        return;
    }
    // anything else: ignored, never answered (as on the player's side)
}

// The LIVE pair, direction and Clear (CTL scope=live); the preset is ONE setting, shared with the player (TrPipe.h).
// handleTranslatorCtl's rules exactly, applied to the other pair.
void Bridge::handleLiveCtl(const std::wstring& mode, const std::wstring& preset, bool clear,
                           const std::wstring& langA, const std::wstring& langB) {
    if (!translator_) return;
    auto oneOf = [](const std::wstring& v, std::initializer_list<const wchar_t*> ok) {
        for (const wchar_t* k : ok) if (v == k) return true;
        return false;
    };
    std::wstring m = mode;
    if      (m == L"en2zh") m = L"a2b";
    else if (m == L"zh2en") m = L"b2a";
    if (oneOf(m, { L"auto", L"a2b", L"b2a" })) {
        liveMode_ = m;
        translator_->setLiveMode(wideToUtf8(m));
    }
    if (oneOf(preset, { L"auto", L"quality", L"balanced", L"light" }))
        translator_->setPreset(wideToUtf8(preset));
    // One half may arrive alone: the other is the app's current belief, and the two travel TOGETHER (a pair or
    // nothing); the same language twice is not a pair -- the engine would refuse it and the mirror would lie.
    std::wstring a, b;
    const bool okA = trLangCode(langA, a), okB = trLangCode(langB, b);
    if (okA || okB) {
        const std::wstring na = okA ? a : liveLangA_;
        const std::wstring nb = okB ? b : liveLangB_;
        if (na != nb && !na.empty() && !nb.empty()) {
            liveLangA_ = na;
            liveLangB_ = nb;
            translator_->setLivePair(wideToUtf8(na), wideToUtf8(nb));
        }
    }
    if (clear) {
        // W5: a Clear ENDS THE SESSION. The user wiping the screen means "that conversation is over", and the next line
        // belongs to the next document -- appending it to the old file would hand them one .docx with two meetings.
        // The file itself stays on disk; only our handle on it is dropped. The engine drops its queued live segments.
        translator_->askClear();
        endLiveSession();
        postToLive(L"{\"type\":\"translatorState\",\"cleared\":true}");
    }
    sendTranslatorState(/*force=*/true);
}

// The session ends (a Clear): the scrollback, the journal's handle, the colours and the strips' voices -- the next line
// starts a new document, colour 1, voice 1.
// Fix round: what the ended journal is still owed gets one last try (RVB-12), and app.txt forgets it (RVB-1: a restart
// takes back the CURRENT session only -- a Clear is "that conversation is over").
void Bridge::endLiveSession() {
    if (!trJournalOwed_.empty()) writeJournalPending();
    trJournalOwed_.clear();
    trJournalSeps_ = 0;
    trJournalWarned_ = false;
    translatorLog_.clear();
    trSessionPath_.clear();
    trSessionLines_ = 0;
    spkColor_.clear();
    spkNextColor_ = 0;
    spkVoices_.clear();
    setLiveSessionTag(std::string());
}

// liveConfig{inputs:[{key, label, chan, gainDb}]}: the strips AS EDITED (0..kMaxLiveInputs), sent on every edit. Checked
// before anything moves -- the WHOLE list or nothing:
//   * where each key comes from: the engine's last list (the picker) or a strip already there (one saved in app.txt
//     whose device is unplugged today is not in the list, and must survive an edit of another strip); a `wav:` key (a
//     PATH: the test sources) only for a test instance -- the engine refuses it anyway outside a test, this keeps the
//     rule "no path ever comes from the page" on the app's side too;
//   * the engine's own rules for the set (AppConfig.h liveSetNormal = trpipe::unpackLive).
// Refused = a `warn` and the previous strips re-sent (translatorState.live.inputs: the page's echo). Accepted = persisted
// (app.txt LIVEIN), and sent to the engine at once if listening (LIVE 1 replaces the set: an unchanged strip keeps
// running untouched, a new label only renames what FOLLOWS -- TrPipe.h).
void Bridge::handleLiveConfig(const std::wstring& json) {
    pbjson::Value v;
    std::vector<pbt::trpipe::WireLiveInput> want;
    bool ok = json.size() <= (64u << 10) && pbjson::Reader(json).parse(v);
    const pbjson::Value* list = ok ? v.get(L"inputs") : nullptr;
    ok = list && list->isArr() && list->a.size() <= pbt::trpipe::kMaxLiveInputs;
    bool unknownKey = false;
    if (ok) {
        for (const pbjson::Value& o : list->a) {
            uint64_t ch = 0;
            const pbjson::Value* g = o.get(L"gainDb");
            if (!o.isObj() || !o.u64(L"chan", ch) || ch > 99 || !g || g->kind != pbjson::Value::Kind::Num) { ok = false; break; }
            pbt::trpipe::WireLiveInput in;
            in.key = u8(o.str(L"key"));
            in.label = u8(o.str(L"label"));
            in.chan = static_cast<uint32_t>(ch);
            in.gainDb = g->n;
            bool known = liveSrcKeys_.count(in.key) != 0;
            for (const auto& s : liveStrips_) if (s.key == in.key) known = true;
            if (pbt::trpipe::liveKeyKind(in.key) == "wav") known = pblaunch::isTestInstance();
            // '|' is the LANE separator (TrPipe.h liveLaneId): a key holding one is refused everywhere (the ADDENDUM's
            // RVA-10 -- "in:X|1" next to "in:X" channel 1 would name two strips alike); no engine lists one either
            if (in.key.find('|') != std::string::npos) known = false;
            if (!known) { ok = false; unknownKey = true; break; }
            want.push_back(in);
        }
    }
    std::vector<pbt::trpipe::WireLiveInput> norm;
    if (ok && !pbapp::liveSetNormal(want, norm)) ok = false;
    if (!ok) {
        postLiveWarn(unknownKey ? L"That source is not in the list any more: the change was not applied."
                                : L"That change cannot be applied: 4 sources at most, a name of 64 bytes at most, a trim "
                                  L"between -24 and +24 dB, and one source twice only on two different channels.");
        lastLiveStateJson_.clear();
        sendLiveState(/*force=*/true);       // the previous strips, as the page's echo
        return;
    }
    std::vector<LiveStrip> next;
    for (const auto& in : norm) {
        LiveStrip s;
        s.key = in.key; s.label = in.label; s.chan = in.chan; s.gainDb = in.gainDb;
        // its lamp: INPUT is the DEVICE's (TrPipe.h LANES), so any strip of that device had it
        for (const auto& old : liveStrips_) if (old.key == s.key) { s.state = old.state; s.why = old.why; }
        next.push_back(s);
    }
    // Fix round (RVB-13): a TRIM being dragged arrives every 200 ms -- app.txt is rewritten once it rests (1 s after the
    // last one: uiTick), not five times a second on this thread (the rename's antivirus retry sleeps up to 100 ms).
    // Anything else (a name, a channel, a strip added or removed) is persisted at once, a pending trim with it.
    bool trimOnly = next.size() == liveStrips_.size();
    for (size_t i = 0; trimOnly && i < next.size(); ++i)
        trimOnly = next[i].key == liveStrips_[i].key && next[i].label == liveStrips_[i].label && next[i].chan == liveStrips_[i].chan;
    liveStrips_ = next;
    if (trimOnly) liveCfgSaveDue_ = ::GetTickCount64() + 1000;
    else          { liveCfgSaveDue_ = 0; saveLiveConfig(); }
    if (liveWanted_ && translator_) {
        if (liveStrips_.empty()) {
            // the last strip removed while listening: there is nothing left to listen to
            liveWanted_ = false;
            translator_->setLive(false, {});
            updateLiveGate();
        } else {
            translator_->setLive(true, norm);
        }
    }
    sendTranslatorState(/*force=*/false);
}

// liveListen{on}: the Listen button. On with no strip = a hint, nothing sent; on = LIVE 1 with the strips (the engine
// cancels a file analysis first: the player's gate closes NOW, before the engine's echo); off = LIVE 0. Never at launch
// (D-L8: capturing a microphone takes the user's gesture -- liveWanted_ starts false and is never persisted).
void Bridge::handleLiveListen(bool on) {
    if (!translator_) return;
    if (on && liveStrips_.empty()) {
        postLiveWarn(L"Add a source first.");
        sendLiveState(/*force=*/true);       // the button comes back to the truth: not listening
        return;
    }
    liveWanted_ = on;
    std::vector<pbt::trpipe::WireLiveInput> set;
    if (on) {
        for (const auto& s : liveStrips_) {
            pbt::trpipe::WireLiveInput in;
            in.key = s.key; in.label = s.label; in.chan = s.chan; in.gainDb = s.gainDb;
            set.push_back(in);
        }
    } else {
        for (auto& s : liveStrips_) { s.state.clear(); s.why.clear(); }
    }
    translator_->setLive(on, set);
    updateLiveGate();
    sendTranslatorState(/*force=*/false);
}

void Bridge::sendLiveSources() {
    if (liveSrcList_.empty()) return;
    postToLive(L"{\"type\":\"liveSources\",\"gen\":" + std::to_wstring(liveSrcGen_) + L",\"list\":" + liveSrcList_ + L"}");
}

// translatorState.live (LIVE_PLAN 3.3), in BOTH views' states: on = the ENGINE's echo (the truth: the on-air lamp, the
// player's notice), wanted = the app's wish (the page can say "starting" between the two), the strips with each one's
// LANE ID (TrPipe.h LANES: the name its meter and its speaker go by -- the page matches liveLevels on it), its last
// INPUT state and its sentence.
std::wstring Bridge::liveJson() const {
    std::wstring j = std::wstring(L"{\"on\":") + (trLive_ ? L"true" : L"false")
                   + L",\"wanted\":" + (liveWanted_ ? L"true" : L"false") + L",\"inputs\":[";
    bool first = true;
    for (const auto& s : liveStrips_) {
        wchar_t g[32];
        ::swprintf(g, 32, L"%.1f", s.gainDb);
        j += (first ? L"" : L",") + std::wstring(L"{\"key\":\"") + jsonEscape(utf8ToWide(s.key.c_str()))
           + L"\",\"lane\":\"" + jsonEscape(utf8ToWide(pbt::trpipe::liveLaneId(s.key, s.chan).c_str()))
           + L"\",\"label\":\"" + jsonEscape(utf8ToWide(s.label.c_str())) + L"\",\"chan\":" + std::to_wstring(s.chan)
           + L",\"gainDb\":" + g + L",\"state\":\"" + jsonEscape(utf8ToWide(s.state.c_str())) + L"\""
           + (s.why.empty() ? std::wstring() : L",\"why\":\"" + jsonEscape(utf8ToWide(s.why.c_str())) + L"\"") + L"}";
        first = false;
    }
    return j + L"]}";
}

// The Live view's translatorState: 1.1.0's (the voiceprint stage `spk`, the session's `sessionLines`) with the LIVE pair
// as its mode / langA / langB (D-L4: one pair per mode) and `live`. Diffed on its own.
void Bridge::sendLiveState(bool force) {
    if (!liveView_) return;
    const std::wstring presetReq = translator_ ? utf8ToWide(translator_->preset().c_str()) : std::wstring();
    const std::wstring json = L"{\"type\":\"translatorState\",\"active\":true"
        + std::wstring(L",\"running\":") + (trRunning_ ? L"true" : L"false")
        + L",\"status\":\"" + jsonEscape(trStatus_) + L"\""
        + L",\"preset\":\"" + jsonEscape(trPreset_) + L"\""
        + (presetReq.empty() ? L"" : L",\"presetReq\":\"" + jsonEscape(presetReq) + L"\"")
        + L",\"mode\":\""  + jsonEscape(liveMode_)  + L"\""
        + L",\"langA\":\"" + jsonEscape(liveLangA_) + L"\""
        + L",\"langB\":\"" + jsonEscape(liveLangB_) + L"\""
        + L",\"gpu\":" + (trGpu_ ? L"true" : L"false")
        + L",\"models\":{\"asr\":\"" + jsonEscape(trAsrModel_)
        + L"\",\"mt\":\"" + jsonEscape(trMtModel_) + L"\"}"
        // The voiceprint stage (1.1.0). ALWAYS present: `on:false` is not an error -- the diarization payload is absent
        // and speakers are named by strip, which is still an answer.
        + L",\"spk\":{\"on\":" + std::wstring(trSpkOn_ ? L"true" : L"false")
        + L",\"model\":\"" + jsonEscape(trSpkModel_) + L"\""
        + L",\"speakers\":" + std::to_wstring(trSpkCount_) + L"}"
        // W5: how many finished lines the CURRENT session file holds -- what an export would actually contain.
        + L",\"sessionLines\":" + std::to_wstring(trSessionLines_)
        + L",\"live\":" + liveJson()
        + (trErr_.empty() ? L"" : L",\"err\":\"" + jsonEscape(trErr_) + L"\"")
        + L"}";
    if (!force && json == lastLiveStateJson_) return;
    lastLiveStateJson_ = json;
    postToLive(json);
}

// THE PLAYER'S GATE (TrPlayer.h setLive): closed while the app WANTS to listen or the engine SAYS it does -- so no FILE
// ever races a LIVE 1, and the player's analysis resumes only once the engine confirmed it stopped listening.
void Bridge::updateLiveGate() {
    const bool gate = liveWanted_ || trLive_;
    if (gate == liveGate_) return;
    liveGate_ = gate;
    if (player_) player_->setLive(gate);
}

// At construction (before any page): the strips app.txt holds (LIVEIN, the engine's rules), and the LIVE pair the engine
// will run -- engine.txt's LIVELANGA / LIVELANGB / LIVEMODE, READ only (the engine owns that file); without them, the
// player's pair (the engine seeds its live pair the same way, L1a). The engine's STATE echo decides from then on.
void Bridge::loadLiveConfig() {
    std::string text;
    const std::wstring p = appDataPath(L"app.txt");
    if (!p.empty() && pbtrf::readFileAll(p, text, 1ULL << 20)) {
        for (const auto& in : pbapp::parseLiveInputs(text)) {
            // (fix round, RVB-5: liveConfig's rule on this path too -- a `wav:` strip, a PATH, is a test instance's only:
            // a hand-edited or copied app.txt must not hand the product's engine a LIVE it refuses)
            if (pbt::trpipe::liveKeyKind(in.key) == "wav" && !pblaunch::isTestInstance()) continue;
            LiveStrip s;
            s.key = in.key; s.label = in.label; s.chan = in.chan; s.gainDb = in.gainDb;
            liveStrips_.push_back(s);
        }
    }
    liveMode_ = trMode_;
    liveLangA_ = trLangA_;
    liveLangB_ = trLangB_;
    std::string cfg, a, b, m = "auto";
    std::wstring na, nb;
    const std::wstring cp = appDataPath(L"engine.txt");
    if (!cp.empty() && pbtrf::readFileAll(cp, cfg, 1ULL << 20) && pbapp::livePairFromEngineCfg(cfg, a, b, m)
        && trLangCode(utf8ToWide(a.c_str()), na) && trLangCode(utf8ToWide(b.c_str()), nb) && na != nb) {
        liveLangA_ = na;
        liveLangB_ = nb;
        liveMode_ = utf8ToWide(m.c_str());
    }
}

// app.txt's LIVEIN lines = the strips now (main.cpp writes WINPOS / LIVEPOS in the same file, on the same thread: each
// rewrites only its own lines, AppConfig.h). An EMPTY READ IS NOT AN EMPTY FILE (AudioPB 1.1.0 learnt it on
// translator.txt): a file that exists but could not be read is not rebuilt from nothing -- this save is skipped.
void Bridge::saveLiveConfig() {
    const std::wstring p = appDataPath(L"app.txt");
    if (p.empty()) return;
    std::string old;
    if (!pbtrf::readFileAll(p, old, 1ULL << 20) && ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    std::vector<pbt::trpipe::WireLiveInput> set;
    for (const auto& s : liveStrips_) {
        pbt::trpipe::WireLiveInput in;
        in.key = s.key; in.label = s.label; in.chan = s.chan; in.gainDb = s.gainDb;
        set.push_back(in);
    }
    const std::string body = pbapp::packLiveInputs(old, set);
    if (body != old) pbtrf::writeFileAvSafe(p, body);   // temp + rename with the antivirus retry
}

// A speaker's name, from the LANE ID its line carries (LINE.spkSlot, TrPipe.h LANES): the label of THE strip with that
// lane -- the name the user gave it (LIVE_PLAN 4.3); never the first strip of the same device, which on "canal 1 = moi,
// canal 2 = l'invite" would name the guest after the host -- else the source's own name from the picker's list (a
// one-channel lane: "<name> . ch N", the page's default name), else the bare name after the kind (an application's exe).
// Lane ids are BUILT from {key, chan} and compared, never parsed back. An input key is never shown raw when anything
// better is known.
std::wstring Bridge::stripLabelFor(const std::wstring& lane) const {
    if (lane.empty()) return std::wstring();
    const std::string id = u8(lane);
    for (const auto& s : liveStrips_)
        if (pbt::trpipe::liveLaneId(s.key, s.chan) == id && !s.label.empty()) return w16(s.label);
    for (const auto& src : liveSrcLabels_) {
        if (src.second.empty()) continue;
        if (src.first == id) return w16(src.second);
        for (uint32_t c = 1; c <= pbt::trpipe::kLiveMaxChan; ++c)
            if (pbt::trpipe::liveLaneId(src.first, c) == id) return w16(src.second) + L" \u00B7 ch " + std::to_wstring(c);
    }
    const size_t c = lane.find(L':');
    return c == std::wstring::npos ? lane : lane.substr(c + 1);
}

// Assigned in ORDER OF APPEARANCE and never reassigned: a speaker's colour moving because someone else joined the call
// is the one thing a colour code must not do. (1.1.0, with the page's 6.)
int Bridge::translatorSpeakerColor(const std::wstring& key) {
    auto it = spkColor_.find(key);
    if (it != spkColor_.end()) return it->second;
    const int c = spkNextColor_ % kSpeakerColors;
    ++spkNextColor_;
    spkColor_[key] = c;
    return c;
}

// A (re)loaded Live page gets the scrollback back: FINISHED lines only, verbatim (closes:false and all -- an hour later
// there is no live line for it to keep standing, so it simply appends).
void Bridge::replayTranslatorLog() {
    for (const auto& line : translatorLog_) postToLive(line);
}

// ---- W5 (1.1.0) -- the durable session log, and the Word document made from it ---------------------------------------
// The user's request (17/09): "in the window that shows the translations, I want to be able to save/export everything
// that has been translated, in an Office format". "Everything" is what forces the two halves apart: the window holds
// 200 lines and can be closed, so the only thing that can honestly answer "everything" is a file written as the meeting
// happens.

// Our own JSON, read back with ESCAPES HONOURED (1.1.0: jsonFindString stops at the first quote, so a subtitle holding
// a quote -- written as \" -- came back cut). The flat key scan is safe for the same reason it is on the way out: every
// value is escaped, so a quoted "key" inside a sentence is `\"key\"` on disk and can never look like a field name --
// which is what lets `spk.key` be reached without a nested parser.
static bool trJsonString(const std::wstring& j, const wchar_t* key, std::wstring& out) {
    const std::wstring pat = L"\"" + std::wstring(key) + L"\"";
    size_t k = j.find(pat);
    if (k == std::wstring::npos) return false;
    size_t colon = j.find(L':', k + pat.size());
    if (colon == std::wstring::npos) return false;
    size_t i = j.find(L'"', colon + 1);
    if (i == std::wstring::npos) return false;
    std::wstring v;
    for (++i; i < j.size(); ++i) {
        const wchar_t c = j[i];
        if (c == L'"') { out = v; return true; }      // the UNESCAPED closer, at last
        if (c != L'\\') { v += c; continue; }
        if (++i >= j.size()) break;                   // a trailing backslash = a torn line
        switch (j[i]) {
            case L'"':  v += L'"';  break;
            case L'\\': v += L'\\'; break;
            case L'/':  v += L'/';  break;
            case L'b':  v += L'\b'; break;
            case L'f':  v += L'\f'; break;
            case L'n':  v += L'\n'; break;
            case L'r':  v += L'\r'; break;
            case L't':  v += L'\t'; break;
            case L'u': {
                if (i + 4 >= j.size()) return false;
                unsigned cp = 0;
                for (int d = 1; d <= 4; ++d) {
                    const wchar_t h = j[i + d];
                    unsigned nib;
                    if      (h >= L'0' && h <= L'9') nib = (unsigned)(h - L'0');
                    else if (h >= L'a' && h <= L'f') nib = (unsigned)(h - L'a' + 10);
                    else if (h >= L'A' && h <= L'F') nib = (unsigned)(h - L'A' + 10);
                    else return false;
                    cp = (cp << 4) | nib;
                }
                i += 4;
                // One UTF-16 CODE UNIT, not one character: a pair arrives as two \u escapes and appending both units in
                // order is exactly what rebuilds it.
                v += (wchar_t)cp;
                break;
            }
            default: v += j[i]; break;   // an escape we never write: keep the character itself
        }
    }
    return false;   // no closing quote: the app died mid-write, and this line is not a record
}

// %APPDATA%\SuVtitle\Live sessions -- a FOLDER of its own, not one growing file: a session is a document, and an export
// must never have to guess where one meeting ended.
void Bridge::appendTranslatorSession(const std::wstring& lineJson) {
    if (trSessionPath_.empty()) {
        // Created on the FIRST line, never at start-up: an app that sits idle for a week must not leave a week of empty
        // files behind.
        const std::wstring dir = appDataPath(L"Live sessions");
        if (dir.empty()) { journalFailed(ERROR_PATH_NOT_FOUND); return; }
        ::CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        wchar_t stamp[32] = {};
        ::swprintf(stamp, 32, L"%04u-%02u-%02u_%02u%02u%02u",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        // (fix round: a Clear and a line in the same second must not append the new session to the one just ended)
        std::wstring name = std::wstring(stamp) + L".jsonl";
        for (int n = 2; n < 100 && ::GetFileAttributesW((dir + L"\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES; ++n)
            name = std::wstring(stamp) + L"_" + std::to_wstring(n) + L".jsonl";
        trSessionPath_ = dir + L"\\" + name;
        trSessionLines_ = 0;
        trJournalOwed_.clear();
        trJournalSeps_ = 0;
        trJournalWarned_ = false;
        // FIX ROUND (RVB-1): app.txt remembers it -- a restart (a crash, the window closed, the PC rebooted) takes this
        // session back, its replay and its export, until a Clear.
        setLiveSessionTag(wideToUtf8(name));
    }
    // APPEND, and therefore no temp+rename dance and no antivirus window to dodge (the rename trap only bites a
    // MoveFileEx issued in the scanner's on-write window -- there is no rename here to lose). FILE_APPEND_DATA without
    // GENERIC_WRITE is what makes the write atomic at the end of the file rather than at a pointer we would have to seek;
    // the share flags let the user read the transcript in Notepad while the meeting is still running.
    // FIX ROUND (RVB-12): the line joins what the journal is OWED (nothing, unless a write failed: a full disk, an editor
    // holding the file without sharing it) -- written with it, in order, by writeJournalPending.
    trJournalOwed_ += wideToUtf8(lineJson) + "\n";
    writeJournalPending();
}

// FIX ROUND (RVB-12): write what the journal is owed. Counted only when the bytes actually landed (a count that ran ahead
// of the file would put a number on the tooltip the export could not produce) -- a write that landed in part keeps the
// rest owed, exactly from where it stopped. A failure is said ONCE (journalFailed) and tried again with the next line; the
// debt is bounded (8 MB: hours of talk) -- past it the oldest owed lines go, and that is said too.
bool Bridge::writeJournalPending() {
    if (trJournalOwed_.empty() || trSessionPath_.empty()) return trJournalOwed_.empty();
    HANDLE h = ::CreateFileW(trSessionPath_.c_str(), FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wr = 0, err = 0;
    BOOL ok = FALSE;
    if (h == INVALID_HANDLE_VALUE) {
        err = ::GetLastError();
    } else {
        ok = ::WriteFile(h, trJournalOwed_.data(), (DWORD)trJournalOwed_.size(), &wr, nullptr);
        if (!ok) err = ::GetLastError();
        ::CloseHandle(h);
    }
    if (wr > trJournalOwed_.size()) wr = (DWORD)trJournalOwed_.size();
    size_t nl = 0;
    for (DWORD i = 0; i < wr; ++i) if (trJournalOwed_[i] == '\n') ++nl;
    const size_t seps = std::min(nl, trJournalSeps_);   // (the newline that ends a torn line is no record)
    trJournalSeps_ -= seps;
    trSessionLines_ += static_cast<long long>(nl - seps);
    trJournalOwed_.erase(0, wr);
    if (ok && trJournalOwed_.empty()) {
        if (trJournalWarned_) {
            trJournalWarned_ = false;
            postLiveWarn(L"The session file is written again: the lines it was missing are in it now.");
        }
        return true;
    }
    if (trJournalOwed_.size() > (8u << 20)) {          // bounded: the oldest owed lines go (said)
        const size_t cut = trJournalOwed_.find('\n', trJournalOwed_.size() - (8u << 20));
        trJournalOwed_.erase(0, cut == std::string::npos ? trJournalOwed_.size() : cut + 1);
        trJournalWarned_ = false;
    }
    journalFailed(err ? err : ERROR_WRITE_FAULT);
    return false;
}

// FIX ROUND (RVB-12): a journal write failed -- said once per run of failures: the Live window's warn (the lines on screen
// are not in the session file, hence not in an export, yet) and one line in %LOCALAPPDATA%\SuVtitle\live.log (bounded).
void Bridge::journalFailed(DWORD err) {
    if (trJournalWarned_) return;
    trJournalWarned_ = true;
    wchar_t* sys = nullptr;
    std::wstring why;
    if (::FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                         err, 0, reinterpret_cast<LPWSTR>(&sys), 0, nullptr) && sys) {
        why = sys;
        ::LocalFree(sys);
    }
    while (!why.empty() && (why.back() == L'\n' || why.back() == L'\r' || why.back() == L' ' || why.back() == L'.')) why.pop_back();
    if (why.empty()) why = L"error " + std::to_wstring(err);
    postLiveWarn(L"The session file cannot be written (" + why + L"): the newest lines are not in it yet -- "
                 L"they are kept and written as soon as it works again.");
    wchar_t la[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    const std::wstring dir = std::wstring(la) + L"\\SuVtitle", path = dir + L"\\live.log";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool big = ::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)
                     && (fa.nFileSizeHigh || fa.nFileSizeLow > 256u * 1024u);
    HANDLE f = ::CreateFileW(path.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             big ? CREATE_ALWAYS : OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    char head[64];
    std::snprintf(head, sizeof head, "%04u-%02u-%02u %02u:%02u:%02u ", (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    const std::string out = std::string(head) + "[SuVtitle] the live session file could not be written (error "
                          + std::to_string(err) + "): " + wideToUtf8(trSessionPath_) + "\r\n";
    DWORD wr = 0;
    ::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &wr, nullptr);
    ::CloseHandle(f);
}

// FIX ROUND (RVB-1): app.txt LIVESESSION = the current session's journal ("" = none). main.cpp writes WINPOS / LIVEPOS
// and saveLiveConfig LIVEIN in the same file, on the same thread: each rewrites its own lines only (AppConfig.h). An
// EMPTY READ IS NOT AN EMPTY FILE (saveLiveConfig's rule): an app.txt that exists but could not be read is left alone.
void Bridge::setLiveSessionTag(const std::string& name) {
    const std::wstring txt = appDataPath(L"app.txt");
    if (txt.empty()) return;
    std::string was;
    if (!pbtrf::readFileAll(txt, was, 1ULL << 20) && ::GetFileAttributesW(txt.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const std::string now = pbapp::packLiveSession(was, name);
    if (now != was) pbtrf::writeFileAvSafe(txt, now);   // temp + rename with the antivirus retry
}

// FIX ROUND (RVB-1, the ADDENDUM: "the session survives a restart until Clear"): app.txt's LIVESESSION names the journal
// of the session this app was in when it ended (closed, crashed, the PC restarted). It is taken back: the next line goes
// on appending to it, the Export exports the WHOLE file, the Live page's replay is its last 200 lines, and the speakers
// keep their colours and numbers (a voice of this run is a new one: spkRun_). Streamed and bounded (256 MB, a line of
// 1 MB at most): a meeting of a day is megabytes. A file gone (the user deleted it) = no session: the next line starts one.
void Bridge::reattachLiveSession() {
    std::string text;
    const std::wstring p = appDataPath(L"app.txt");
    if (p.empty() || !pbtrf::readFileAll(p, text, 1ULL << 20)) return;
    const std::string name = pbapp::parseLiveSession(text);
    const std::wstring dir = appDataPath(L"Live sessions");
    if (name.empty() || dir.empty()) return;
    const std::wstring path = dir + L"\\" + utf8ToWide(name.c_str());
    const DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;   // (the user may hold it open: read it anyway)
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, share, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::deque<std::wstring> tail;
    long long count = 0;
    std::string part;
    bool longLine = false;
    unsigned long long total = 0;
    std::map<std::wstring, int> colors;
    std::map<std::wstring, std::vector<std::wstring>> voices;
    int distinct = 0;
    auto record = [&](std::string& one) {
        if (!one.empty() && one.back() == '\r') one.pop_back();
        if (one.size() < 2) return;
        ++count;
        std::wstring j = utf8ToWide(one.c_str());
        // who spoke: the colour the line was given, and the strip's voices in order (OUR keys, built as `slot#voice`,
        // compared with the slot the line names -- never parsed back otherwise)
        std::wstring key, slot;
        long long color = -1;
        if (trJsonString(j, L"key", key) && !key.empty() && jsonFindNumber(j, L"color", color) && color >= 0) {
            if (colors.emplace(key, static_cast<int>(color % kSpeakerColors)).second) ++distinct;
            if (trJsonString(j, L"slot", slot) && !slot.empty() && key.size() > slot.size() + 1
                && key.compare(0, slot.size() + 1, slot + L"#") == 0) {
                const std::wstring voice = key.substr(slot.size() + 1);
                std::vector<std::wstring>& v = voices[slot];
                if (std::find(v.begin(), v.end(), voice) == v.end()) v.push_back(voice);
            }
        }
        tail.push_back(std::move(j));
        if (tail.size() > kTranslatorLogMax) tail.pop_front();
    };
    for (;;) {
        char buf[65536];
        DWORD got = 0;
        if (!::ReadFile(h, buf, (DWORD)sizeof buf, &got, nullptr) || got == 0) break;
        total += got;
        for (DWORD i = 0; i < got; ++i) {
            if (buf[i] != '\n') {
                if (!longLine) part += buf[i];
                if (part.size() > (1u << 20)) { part.clear(); longLine = true; }   // not a line of ours: skipped whole
                continue;
            }
            if (!longLine) record(part);
            part.clear();
            longLine = false;
        }
        if (total > (256ull << 20)) break;
    }
    ::CloseHandle(h);
    trSessionPath_ = path;
    trSessionLines_ = count;
    translatorLog_ = std::move(tail);
    spkColor_ = std::move(colors);
    spkNextColor_ = distinct;
    spkVoices_ = std::move(voices);
    // A torn last line (the app died mid-write): not a record -- the next one must not be glued to it.
    if (!part.empty() || longLine) { trJournalOwed_ = "\n"; trJournalSeps_ = 1; }
}

// FIX ROUND (RVB-13): the debounced LIVEIN save, written now if one is due (uiTick when it is, shutdown / the end of the
// Windows session at once).
void Bridge::flushLiveConfig() {
    if (!liveCfgSaveDue_) return;
    liveCfgSaveDue_ = 0;
    saveLiveConfig();
}

// ONE ack shape for both outcomes: ok:true carries path + lines, ok:false a sentence a human can read -- and the
// dialog's Cancel is an ok:false with err "cancelled", which the page shows quietly (LIVE_PLAN 3.3: silence).
void Bridge::postTranslatorExported(bool ok, const std::wstring& pathOrErr, long long lines) {
    std::wstring j = L"{\"type\":\"translatorExported\",\"ok\":";
    j += ok ? L"true" : L"false";
    if (ok) j += L",\"path\":\"" + jsonEscape(pathOrErr) + L"\",\"lines\":" + std::to_wstring(lines);
    else    j += L",\"err\":\"" + jsonEscape(pathOrErr) + L"\"";
    j += L"}";
    postToLive(j);
}

void Bridge::handleTranslatorExport(const std::wstring& format) {
    // A file dialog is already up (its modal loop re-entered us -- the PLAYER's Open, which does not disable the Live
    // window): one at a time, and SAID (fix round, RVB-15: the click was lost in silence).
    if (trDialogBusy_) {
        postTranslatorExported(false, L"a file dialog is open: finish it first, then export again", 0);
        return;
    }
    // Only .docx exists. An unknown format is REFUSED by name rather than silently treated as docx: a page asking for
    // "xlsx" must learn that it did not happen, not receive a Word file it will label as a spreadsheet.
    if (!format.empty() && format != L"docx") {
        postTranslatorExported(false, L"only Word documents (.docx) can be exported for now", 0);
        return;
    }
    if (trSessionPath_.empty()) {
        postTranslatorExported(false, L"nothing to export yet", 0);
        return;
    }

    // ---- read the session back ------------------------------------------------
    // Sharing everything: WE are still appending to this file, and a read that locked it would make the export kill
    // the very transcript it is saving. Bounded (256 MB: weeks of talk), like every file this app reads.
    HANDLE h = ::CreateFileW(trSessionPath_.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        postTranslatorExported(false, L"the session file could not be opened", 0);
        return;
    }
    std::string raw;
    for (;;) {
        char buf[16384];
        DWORD got = 0;
        if (!::ReadFile(h, buf, (DWORD)sizeof buf, &got, nullptr) || got == 0) break;
        raw.append(buf, got);
        if (raw.size() > (256u << 20)) break;
    }
    ::CloseHandle(h);
    raw += trJournalOwed_;   // (fix round, RVB-12: what a failing write still owes the file is the session's too)

    std::vector<pbdocx::Line> lines;
    size_t at = 0;
    while (at < raw.size()) {
        size_t nl = raw.find('\n', at);
        std::string one = raw.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = (nl == std::string::npos) ? raw.size() : nl + 1;
        if (!one.empty() && one.back() == '\r') one.pop_back();
        if (one.size() < 2) continue;
        // OUR OWN JSON, parsed by the helpers that wrote it. A torn last line (the app was killed mid-write) simply
        // fails to yield a `src` and is skipped -- which is why the record is a line-per-object file and not one big
        // array that a crash would invalidate.
        const std::wstring j = utf8ToWide(one.c_str());
        pbdocx::Line l;
        long long t = 0;
        if (jsonFindNumber(j, L"t", t)) l.tEpochMs = t;
        trJsonString(j, L"lang",    l.lang);
        trJsonString(j, L"dstLang", l.dstLang);
        trJsonString(j, L"src",     l.src);
        trJsonString(j, L"dst",     l.dst);
        jsonFindBool(j, L"pass",    l.pass);
        // spk is a nested object and this is a FLAT scanner: it finds the first "key" in the line, which is the
        // speaker's -- there is no other, and a quoted "key" inside a subtitle is escaped on disk (see trJsonString).
        trJsonString(j, L"key",   l.spkKey);
        trJsonString(j, L"label", l.spkLabel);
        if (l.src.empty() && l.dst.empty()) continue;
        lines.push_back(std::move(l));
    }
    if (lines.empty()) {
        postTranslatorExported(false, L"nothing to export yet", 0);
        return;
    }

    // ---- the document's two header lines --------------------------------------
    auto stampOf = [](long long epochMs, const wchar_t* fmt, bool date) {
        SYSTEMTIME lt{};
        if (epochMs > 0) {
            // Unix ms -> FILETIME (100 ns since 1601). 11644473600 s is the gap between the two epochs; getting it
            // wrong puts the meeting in the 17th century.
            const unsigned long long ft100 = (unsigned long long)(epochMs + 11644473600000LL) * 10000ULL;
            FILETIME ft{};
            ft.dwLowDateTime  = (DWORD)(ft100 & 0xFFFFFFFFULL);
            ft.dwHighDateTime = (DWORD)(ft100 >> 32);
            FILETIME local{};
            SYSTEMTIME ut{};
            if (::FileTimeToLocalFileTime(&ft, &local) && ::FileTimeToSystemTime(&local, &ut)) lt = ut;
        }
        if (lt.wYear == 0) ::GetLocalTime(&lt);
        wchar_t b[32] = {};
        if (date) ::swprintf(b, 32, fmt, lt.wYear, lt.wMonth, lt.wDay);
        else      ::swprintf(b, 32, fmt, lt.wHour, lt.wMinute);
        return std::wstring(b);
    };
    const long long t0 = lines.front().tEpochMs;
    pbdocx::Doc info;
    info.title = L"SuVtitle Live \u2014 " + stampOf(t0, L"%04u-%02u-%02u", true);
    info.subtitle = L"Session started " + stampOf(t0, L"%02u:%02u", false)
                  + L", " + std::to_wstring((long long)lines.size()) + L" lines, "
                  + pbdocx::languageName(liveLangA_) + L" \u2194 " + pbdocx::languageName(liveLangB_);

    std::string bytes;
    if (!pbdocx::buildTranscript(info, lines, bytes) || bytes.empty()) {
        postTranslatorExported(false, L"the document could not be built", 0);
        return;
    }

    // ---- where does it go? (the native Save dialog) ---------------------------
    // The user's OWN machine, the user's OWN click, a plain file dialog. Nothing is written before they name a place.
    // FIX ROUND (R5, tests only): a TEST INSTANCE -- an ISOLATED profile, never the user's: pblaunch::isTestInstance()
    // exits on the real one -- may answer the dialog with SUVTITLE_TEST_EXPORT=<a .docx path> ("cancel" = the dialog's
    // Cancel): an automated run then raises no OS window that could take the user's foreground. The product never reads it.
    {
        wchar_t tv[MAX_PATH] = {};
        const DWORD tn = pblaunch::isTestInstance() ? ::GetEnvironmentVariableW(L"SUVTITLE_TEST_EXPORT", tv, MAX_PATH) : 0;
        if (tn > 0 && tn < MAX_PATH) {
            const std::wstring answer = tv;
            if (answer == L"cancel") { postTranslatorExported(false, L"cancelled", 0); return; }
            if (!pbtrf::writeFileAvSafe(answer, bytes, true)) {
                postTranslatorExported(false, L"the document could not be saved there", 0);
                return;
            }
            postTranslatorExported(true, answer, (long long)lines.size());
            return;
        }
    }
    IFileSaveDialog* dlg = nullptr;
    if (FAILED(::CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg))) || !dlg) {
        postTranslatorExported(false, L"the Save dialog could not be opened", 0);
        return;
    }
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dlg->SetTitle(L"Save the translated session");
    const COMDLG_FILTERSPEC filter[] = { { L"Word document (*.docx)", L"*.docx" } };
    dlg->SetFileTypes(1, filter);
    dlg->SetFileTypeIndex(1);
    dlg->SetDefaultExtension(L"docx");     // a name typed without one still lands as .docx
    {
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        wchar_t name[96] = {};
        ::swprintf(name, 96, L"SuVtitle Live %04u-%02u-%02u %02u-%02u.docx",
                   st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        dlg->SetFileName(name);
    }
    {
        // Documents, not the last folder Windows remembers: a transcript is a document.
        const std::wstring docs = trDocumentsDir();
        IShellItem* item = nullptr;
        if (!docs.empty() && SUCCEEDED(::SHCreateItemFromParsingName(docs.c_str(), nullptr, IID_PPV_ARGS(&item))) && item) {
            dlg->SetFolder(item);
            item->Release();
        }
    }
    std::wstring target;
    // The window the request came FROM owns the dialog (1.1.0, wave WC): owned by the player -- hidden, or behind -- it
    // would open BEHIND an always-on-top Live window.
    const HWND owner = (exportOwner_ && ::IsWindow(exportOwner_)) ? exportOwner_ : dialogOwner();
    trDialogBusy_ = true;
    const HRESULT shown = dlg->Show(owner);
    trDialogBusy_ = false;
    if (SUCCEEDED(shown)) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                target = path;
                ::CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    if (target.empty()) {
        // Cancel is not a failure and must not look like one: the ack still goes out (a page waiting on one would
        // otherwise show "exporting..." for ever) with the one error string the page is told to swallow.
        postTranslatorExported(false, L"cancelled", 0);
        return;
    }

    // ---- write it: temp + rename, the house's antivirus dance -----------------
    // A rename issued straight after the write sits in Bitdefender's on-write scan window and fails on this machine:
    // pbtrf::writeFileAvSafe retries it 5 x 20 ms, then writes in place -- a half-written .docx the user can see and
    // export again is recoverable; an orphaned .tmp next to a file that never appeared is not. (The user confirmed any
    // overwrite in the dialog: replace is his.)
    if (!pbtrf::writeFileAvSafe(target, bytes, true)) {
        postTranslatorExported(false, L"the document could not be saved there", 0);
        return;
    }
    postTranslatorExported(true, target, (long long)lines.size());
}

// ============================================================================
// THE UPDATE BANNER (2026-11-03). The user: when he publishes a new installer, SuVtitle tells whoever uses it, who can get
// the new version at once -- and its installer replaces the old one. AudioPB's protocol (its bridge's update.check /
// update.status / update.open and NextaleUpdate.cpp), in SuVtitle's way:
//   * the NEXTALE release tool publishes https://www.nextale-audio.com/updates/suvtitle.json = {version, url, notes}; the app
//     reads it ONCE per start -- at the page's first update.check, after its `ready`, so never during the start itself -- on
//     a worker thread (NextaleUpdate.h: 10 s timeouts, 2xx only, anything wrong = nothing to say, silently: the 404 of a
//     manifest not published yet included);
//   * update.status{enabled, available, version, notes, later, current, skipped} answers every update.check (a page reloaded
//     for a new folder asks again and is told the same: the site is not asked twice), then the worker's answer, Later, Skip
//     and the toggle;
//   * SuVtitle is free software (GPL); NEXTALE Audio sells a ready-made installer. update.open{} opens SuVtitle's PAGE on
//     the site, where that installer is: NextaleUpdate.h pageUrl(<the offered version>, <this version>) =
//     https://www.nextale-audio.com/suvtitle?update=<new>&from=<this>, ONE constant and two versions versionOk checked --
//     built HERE, checked again at the launch (urlAllowed: the site only), never an address the page hands back (D1) nor
//     the manifest's url (checked, not opened). update.opened{ok, url, test?} says how it went -- `url` is that page's
//     address (what the strip names when the browser could not be opened);
//   * update.later{} puts the banner away for this run; update.skip{} puts THIS version away (app.txt UPDATESKIP: a newer one
//     is offered again) until update.unskip{} -- the Updates dialog's "Offer it again", which still names the skipped version
//     (review 03/11, S5); update.enable{on} is the opt-out (app.txt UPDATECHECK, absent = on) -- turned on, the site is asked
//     at once if this run has not asked yet;
//   * a TEST INSTANCE never asks the site -- only the loopback server its test names in SUVTITLE_TEST_UPDATE_URL, else no
//     check at all -- and never opens the user's browser (update.opened says what would have opened, with test:true).
// UI THREAD ONLY, but for the worker, which holds the box and the window's HWND -- never `this` (H1).
// ============================================================================
void Bridge::loadUpdatePrefs() {
    std::string text;
    const std::wstring p = appDataPath(L"app.txt");
    if (!p.empty() && pbtrf::readFileAll(p, text, 1ULL << 20)) {
        const pbapp::UpdatePrefs u = pbapp::parseUpdatePrefs(text);
        updEnabled_ = u.check;
        updSkip_ = u.skip;
    }
}

// app.txt's UPDATECHECK / UPDATESKIP lines = the prefs now -- saveModels' rule: an EMPTY READ IS NOT AN EMPTY FILE (one that
// exists but could not be read is left alone), written only when it changes, through the AV-safe write (temp + rename, the
// antivirus retry). main.cpp's WINPOS / LIVEPOS and the bridge's other lines are kept (same thread, own lines).
void Bridge::saveUpdatePrefs() {
    const std::wstring cfg = appDataPath(L"app.txt");
    if (cfg.empty()) return;
    std::string before;
    if (!pbtrf::readFileAll(cfg, before, 1ULL << 20) && ::GetFileAttributesW(cfg.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    pbapp::UpdatePrefs u;
    u.check = updEnabled_;
    u.skip = updSkip_;
    const std::string packed = pbapp::packUpdatePrefs(before, u);
    if (packed == before) return;              // (unchanged: no write, no antivirus window)
    pbtrf::writeFileAvSafe(cfg, packed);       // temp + rename with the antivirus retry
}

// Once per start, and only when the user lets it: the manifest on a worker thread. The worker holds the box and the window's
// HWND (never `this`: it may outlive the Bridge) and rings kWmTranslatorPoll -- pumpTranslator drains (uiTick: within 1 s
// even if the ring is lost).
void Bridge::startUpdateCheck() {
    if (updStarted_ || !updEnabled_ || !updBox_) return;
    std::wstring url = nextale::Update::kManifestUrl;
    if (pblaunch::isTestInstance()) {
        // A TEST NEVER ASKS THE SITE: only a loopback server its test names (http://127.0.0.1:<port>/..., localhost) --
        // none named, no check at all.
        const std::wstring t = pblaunch::envOf(L"SUVTITLE_TEST_UPDATE_URL");
        if (!nextale::Update::loopbackUrlOk(t)) return;
        url = t;
    }
    updStarted_ = true;
    std::shared_ptr<UpdBox> box = updBox_;
    const HWND h = hostWnd_.load();
    std::thread([box, h, url] {
        nextale::Update::Info info = nextale::Update::check(SUV_VERSION_STR, url);
        {
            std::lock_guard<std::mutex> lk(box->mx);
            box->info = std::move(info);
            box->done = true;
        }
        if (h && !box->quit.load()) ::PostMessageW(h, kWmTranslatorPoll, 0, 0);
    }).detach();
}

void Bridge::drainUpdate() {
    if (!updStarted_ || updDone_ || !updBox_) return;
    nextale::Update::Info info;
    {
        std::lock_guard<std::mutex> lk(updBox_->mx);
        if (!updBox_->done) return;
        info = updBox_->info;
    }
    updDone_ = true;
    if (info.available) {
        updVersion_ = info.version;
        updNotes_ = info.notes;                // (info.url, the manifest's page: checked by evaluate, never opened)
    }
    postUpdateStatus();
}

// update.status -- to a page that said `ready` only (one still loading asks with its own update.check, after its `ready`).
// `available` = a newer version is known, not skipped, and the check is on; `skipped` = it is known but the user skipped it
// (review 03/11, S5: the Updates dialog still names it, and can offer it again) -- either way `version` / `notes` come with it;
// `later` = put away for this run; `current` = this version.
void Bridge::postUpdateStatus() {
    if (!webview_ || !trWin_.canPost()) return;
    const bool known = updEnabled_ && !updVersion_.empty();
    const bool skipped = known && updVersion_ == updSkip_;
    const bool avail = known && !skipped;
    JsonW j;
    j.obj().kv(L"type", L"update.status").kv(L"enabled", updEnabled_).kv(L"available", avail)
           .kv(L"current", utf8ToWide(SUV_VERSION_STR));
    if (known)
        j.kv(L"version", utf8ToWide(updVersion_.c_str())).kv(L"notes", utf8ToWide(updNotes_.c_str())).kv(L"later", updLater_)
         .kv(L"skipped", skipped);
    j.end();
    postToTranslatorView(j.take());
}

// update.open{}: SuVtitle's page -- pageUrl(the offered version, this version), NextaleUpdate.h: the app's own constant and the
// two versions, built here, launched only through openInBrowser's guard (nothing else is ever opened: no address of the
// manifest's or the page's). A TEST INSTANCE never opens the user's browser: it says what would have happened (test:true).
// update.opened's `url` is the address opened (the strip names it when the browser could not be opened).
// (review 03/11) S5: a SKIPPED version opens too -- only the Updates dialog offers it, on purpose. N3: a repeat within 2 s (a
// double-click; the banner's button then the dialog's) is answered with the last update.opened -- nothing is opened a second
// time (two browser tabs).
void Bridge::openUpdatePage() {
    if (!updEnabled_ || updVersion_.empty()) return;   // nothing known: nothing to open
    const ULONGLONG now = ::GetTickCount64();
    if (updOpenAt_ != 0 && now - updOpenAt_ < 2000 && !updOpenJson_.empty()) {
        postToTranslatorView(updOpenJson_);
        return;
    }
    const std::string url = nextale::Update::pageUrl(updVersion_, SUV_VERSION_STR);   // ("" = refused: not a version)
    const bool test = pblaunch::isTestInstance();
    const bool ok = !url.empty() && (test || nextale::Update::openInBrowser(url));
    JsonW j;
    j.obj().kv(L"type", L"update.opened").kv(L"ok", ok).kv(L"url", utf8ToWide(url.c_str()));
    if (test) j.kv(L"test", true);
    j.end();
    updOpenAt_ = now;
    updOpenJson_ = j.take();
    postToTranslatorView(updOpenJson_);
}

} // namespace patchbus
