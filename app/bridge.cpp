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

#include <algorithm>
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
};
// WH: probes and extractions of the film's own tracks (MediaTracks.h) run on WORKER threads -- a probe reads a few KB
// (measured 0.1-5 ms), but a header walk of a 2 GB film whose Cues cannot be used reads it all (measured 0.25-1.3 s,
// tracks_smoke), and the UI thread must never wait for a disk. Their answers wait here, like a folder scan's, until the
// UI thread drains them. Shared, never owned by `this`: a worker still running when the tray quits finds it alive; every
// worker looks at its cancel flag in each of its loops (the tray's end sets them all).
struct Bridge::TrackBox {
    std::mutex mx;
    struct Probe { uint64_t req = 0; std::shared_ptr<const pbmedia::Info> info; };
    struct Sub { uint64_t req = 0; std::shared_ptr<const pbmedia::SubResult> res; };
    std::vector<Probe> probes;
    std::vector<Sub>   subs;
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
                return S_OK;
            }).Get(),
        &token);
    webMsgToken_ = token.value;

    // THE ENGINE. AudioPB started its helper when the user added the Translator card (and the plain player said "Add
    // the Translator card to the desk to analyse files" without one); SuVtitle IS the analysis, so it starts here,
    // once, and the supervisor respawns it if it dies. A test instance's engine runs on the CPU: a test never takes
    // the user's GPU (SPLIT_PLAN section 4).
    if (translator_) {
        if (pblaunch::isTestInstance()) translator_->setSpawnArgs(L"--cpu");
        translator_->start();
    }
    running_.store(true);
    if (player_) player_->setHelperAvailable(translator_ != nullptr);

    // FILE MODE: a fresh view has no mapping at all. BEFORE its page's first navigation it gets the
    // proxy folder and the player's folders (the current item's and the next ones'): a mapping set
    // after a page loaded is invisible to it (measured, WC review and WE STEP0 M1). The window was
    // just created, so it is on screen.
    trMappedHosts_.clear();
    mapTranslatorProxyFolder();
    trWin_.attached();                       // on screen; its page has not said `ready` yet
    if (player_) {
        player_->windowAttached();
        player_->windowShown(true);
        player_->view("file");               // SuVtitle: the page has the File mode only (S7)
    }
}

void Bridge::shutdown() {
    if (!running_.exchange(false)) return;   // never attached, or already shut down
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
        // ITS page is live from now on: a pending "show the File tab" reaches it once, and is forgotten (C1).
        trWin_.ready();
        postTranslatorWindowState();
        if (player_) {
            player_->windowReady();
            player_->view("file");   // SuVtitle: whatever the page says, there is no other view (S7)
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
        // Each field only when the page sent it: one click changes one thing.
        bool b = false;
        long long ms = 0; jsonFindNumber(json, L"ms", ms);
        if (json.find(L"\"translate\"") != std::wstring::npos && jsonFindBool(json, L"translate", b))
            player_->setTranslate(b, ms > 0 ? (uint64_t)ms : 0);
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
        // AudioPB: a Clear ended the LIVE session (its log, its session file). SuVtitle has neither: the CTL still
        // carries it (the engine drops queued live segments -- it has none) and the page still hears `cleared`.
        translator_->askClear();
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
        + (trErr_.empty() ? L"" : L",\"err\":\"" + jsonEscape(trErr_) + L"\"")
        + L"}";
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
    pollTranslatorOnce();
}

// Warnings, then STATE, then the file messages, in the order the engine sent them.
void Bridge::pollTranslatorOnce() {
    if (!translator_) return;

    // The supervisor's own warnings ("keeps failing to start", "stopped responding; restarting it"). AudioPB showed
    // them on the DESK ({type:"warn"}); SuVtitle has no desk, and its page shows translatorFileNotice -- so there.
    for (auto& w : translator_->pollWarnings()) postTranslatorFileNotice(w);

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
        dirty = true;
        // WE: the pair and direction RUNNING now are what a new file job (and the badges' cache
        // keys) are made with -- the echo, never the app's optimistic copy.
        if (player_) player_->setPair(wideToUtf8(trLangA_), wideToUtf8(trLangB_), wideToUtf8(trMode_));
    }
    const bool run = translator_->running();
    if (run != trRunning_) { trRunning_ = run; dirty = true; }
    if (dirty) sendTranslatorState(/*force=*/false);

    using MsgKind = pbr::TranslatorManager::Msg::Kind;
    for (auto& msg : translator_->pollMessages()) {
        // ---- FILE MODE (WC/WE): a recording's progress, cues and end, to the player --------
        // Same ordered queue as everything else; each cue goes to the window as it arrives, the
        // session state once at the end of the drain (player_->flush, after every cue it closes).
        // A synthesized FDONE (the engine died) is said to be one: it vouches for nothing.
        if (msg.kind == MsgKind::FileProg || msg.kind == MsgKind::FileCue
            || msg.kind == MsgKind::FileDone || msg.kind == MsgKind::ProxyDone) {
            if (!player_) continue;
            if (msg.kind == MsgKind::FileProg)      player_->onProg(msg.fprog);
            else if (msg.kind == MsgKind::FileCue)  player_->onCue(msg.cue);
            else if (msg.kind == MsgKind::FileDone) player_->onDone(msg.fdone, msg.synth);
            else                                    player_->onProxyDone(msg.pdone);
            continue;
        }
        // LINE / PART / END: the LIVE half of TrPipe.h. SuVtitle's engine is FILE MODE only (S5) and never sends
        // them; one that arrives anyway has no surface here (AudioPB keeps the live translator): dropped.
    }
    if (player_) player_->flush();   // one translatorFile per drain, however many FPROGs
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

std::string  u8(const std::wstring& w) { return pbtrf::toUtf8(w); }
std::wstring w16(const std::string& s) { return pbtrf::fromUtf8(s); }

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
    for (auto& p : probes) player_->onTracks(p.req, p.info);
    for (auto& s : subs) player_->onSubExtracted(s.req, s.res);
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
        info.subtitle = L"Transcript"
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
        name = base + L" - transcript (" + tgt + L").docx";
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
    dlg->SetTitle(format == L"docx" ? L"Save the transcript" : L"Save the subtitles");
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
}

} // namespace patchbus
