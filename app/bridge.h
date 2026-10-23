// =============================================================================
// SuVtitle - app/bridge.h   (the bridge between the window's page, the player core and the engine)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/bridge.h + ui/bridge.cpp at commit 641f1aa, branch APB_TRANSLATOR
// "v1.0.6") for the SuVtitle split (docs/SPLIT_PLAN.md S2/S3/S6). AudioPB's Bridge was the whole tray's: the fabric,
// the router, the FX buses, the VST3 catalogue, presets, the licence, updates, OBS, the print card -- and, since the
// waves WC..WH, the Translator's FILE MODE and its player. SuVtitle keeps the last part ONLY, carved out with its code
// unchanged wherever it could be: the PlayerHost (the core's hands), the translatorFile* / player* / subEdit /
// subSaveAs / translatorFileExport handlers, the folder scans and the track workers, the subtitle files and their
// editor, the automatic per-language .srt, translatorCtl / translatorState for the PAIR and the engine's state, and
// the engine supervisor (TranslatorManager). What went: everything of the desk, and the LIVE translator (its card,
// its lines and partials, its scrollback, its session file and Word export) -- AudioPB keeps those (S9).
//
// WHAT CHANGED BECAUSE THERE IS ONE WINDOW. AudioPB had two views on one bus: the desk (index.html) and the
// translator window (translator.html). SuVtitle has ONE: its window IS the player. So:
//   * attach() takes THE view and its HWND (the dialog owner, the wake target); postToWeb / postToTranslatorView /
//     the core's postBoth and postWindow all reach it; the core's postMain (the desk's card while the window's page
//     reloads) has no desk to reach -- the reloading page gets the whole state on its `ready`, as it always did.
//   * The engine is ALWAYS wanted (there is no card to add): it starts at attach and the core knows it is there.
//   * The core's view is "file", always: the page has no Live tab (S7), and the pre-analysis runs on File only.
//   * The page is never hidden while the app runs (closing the window quits): translatorWindow{shown:true}.
// Names stay (S3): patchbus::Bridge, the kWm* message values, every message type of the bridge <-> page contract.
//
// C++17: std::thread + std::atomic. UI THREAD ONLY unless a member says otherwise.
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "TrWindowState.h" // pbplayer::WindowState (pure, light: the window's posted state)

struct ICoreWebView2;                       // forward-declared WebView2 interface
namespace pbr { class TranslatorManager; }  // the engine supervisor (TranslatorManager.h)
// The PLAYER (wave WE): its state machine is TrPlayer.h. Forward-declared so main.cpp's TU stays light.
namespace pbplayer { class Player; struct CueSet; }
namespace pbt { namespace trpipe { struct WireCue; } }   // WG: a language's .srt is written from a cue list

namespace patchbus {

struct TrExtCue;   // WG: a cue of the subtitle file being edited (bridge.cpp)

class Bridge {
public:
    Bridge();
    ~Bridge();

    Bridge(const Bridge&) = delete;
    Bridge& operator=(const Bridge&) = delete;

    // THE view (SuVtitle's one window) and its HWND: the owner of every dialog the page asks for, and where the
    // engine's wake and the workers' answers are posted. Before the page's first navigation (the core maps the
    // player's folders here: a mapping set after a page loaded is invisible to it -- measured, WC review).
    void attach(ICoreWebView2* webview, HWND hwnd);
    void shutdown();

    void postToWeb(const std::wstring& json);   // thread-safe (marshals to the UI thread)
    void drainWebQueue();                       // UI thread pumps this on kWmBridge

    // WE (player v2): FULL SCREEN IS THE WINDOW (WE_PLAN_v2 D3). The page asks with {type:"translatorFullscreen", on};
    // the bridge forwards it to main.cpp (kWmTranslatorWindow wParam 2), whose setFullscreen makes the window
    // borderless on its monitor and calls THIS back with the outcome; the page is told {type:"translatorWindow",
    // shown, fullscreen}. The window owns the fact, the bridge mirrors it, a page reloaded mid-film gets it back.
    void translatorFullscreenChanged(bool on);
    // RV4-7 (2026-10-23): ALWAYS ON TOP, the same way -- main.cpp reads the window's real WS_EX_TOPMOST back after each
    // pin request (a refusal included), after a change made by another program, and once at the start; the page's pin
    // follows translatorWindow.topmost.
    void translatorTopmostChanged(bool on);
    // WG: SuVtitle AS A WINDOWS PLAYER -- the paths of `--play` (Explorer's double-click, "Open with", or another
    // SuVtitle forwarding them through WM_COPYDATA): each checked (TrLaunch.h), folders scanned, media played, a
    // subtitle file for what plays. UI thread (main.cpp's kWmPlayPending).
    void playPaths(const std::vector<std::wstring>& paths);
    // The one-shot "show the File tab" (TrWindowState.h): kept from AudioPB's tray item for parity -- --play asks it.
    void showPlayer();
    // A file dialog of the player is up (its modal loop is pumping): main.cpp holds --play paths until it closes
    // (review of WG #11 -- a double-click in Explorer then switched the film under the subtitle picker).
    bool dialogBusy() const { return trDialogBusy_; }
    // The Windows session is ending (WM_QUERYENDSESSION / WM_ENDSESSION): the player's queue, positions and the
    // vouched part of a running analysis are written now (review of WE, P8).
    void persistPlayer();
    // main.cpp watches for this: wParam 0 = bring the window up (restore it; in front unless a test instance),
    //                            wParam 1 = always-on-top from lParam (0/1),
    //                            wParam 2 = full screen on/off from lParam (0/1) -- WE.
    static constexpr UINT kWmTranslatorWindow = WM_APP + 0x52;
    // ...and this one is the WAKE (wave W1): the manager's worker posts it the moment anything lands in its queue (and
    // the scan / track workers when they finish); the window proc answers with pumpTranslator(). uiTick still drains
    // too: the post is a shortcut, never the contract (a lost message must cost a second, not a cue).
    static constexpr UINT kWmTranslatorPoll = WM_APP + 0x53;
    void pumpTranslator();   // UI THREAD ONLY (the window proc, and uiTick)

    // The 1 Hz UI-thread tick (main.cpp's WM_TIMER): the engine's safety-net drain, the player's clock (checkpoints,
    // the queue and positions written, the pre-analysis), the edited subtitle file's 2 s save.
    void uiTick();

    static constexpr UINT kWmBridge = WM_APP + 0x51;
    void setHostWindow(HWND hwnd) {
        hostWnd_ = hwnd;
        if (sink_) { ::EnterCriticalSection(&sink_->cs); sink_->hwnd = hwnd; ::LeaveCriticalSection(&sink_->cs); }
    }

private:
    // ---- inbound from the page ----
    void handleWebMessage(const std::wstring& json);

    // ---- helpers ----
    // %APPDATA%\SuVtitle\<file> (the folder created): player.txt, player-resume.txt, player-audio.txt; engine.txt is
    // READ only (the engine's). AudioPB's graphFilePath, renamed with the desk it named.
    std::wstring appDataPath(const wchar_t* file) const;

    ICoreWebView2* webview_ = nullptr;   // THE view (not owned); UI-thread only
    int64_t        webMsgToken_ = 0;
    // Atomic since W1: the engine worker's wake lambda reads it off the UI thread.
    std::atomic<HWND> hostWnd_{ nullptr };
    std::atomic<bool> running_{ false };  // attach() .. shutdown()
    bool started_ = false;                // startup() ran (the queue loaded)
    void startup();                       // review of WG #9: the queue, once, in the process that IS the app

    // ---- THE ENGINE (AudioPB's "Translator helper"): SuVtitleEngine.exe, supervised ----------------------------
    // Always wanted in SuVtitle. The pair, the direction and the quality preset are the ENGINE's (it persists them in
    // engine.txt, S11): the app asks with translatorCtl -> CTL, and mirrors what the engine's STATE echoes.
    void handleTranslatorCtl(const std::wstring& mode, const std::wstring& preset, bool clear,
                             const std::wstring& langA, const std::wstring& langB);
    void pollTranslatorOnce();
    // W1 coalescing: true while a kWmTranslatorPoll is in flight. A burst is ONE post -- the worker sets it, the UI
    // thread clears it before draining (a message queued DURING a drain buys its own post).
    std::atomic<bool> trPollPending_{ false };
    void sendTranslatorState(bool force);
    std::unique_ptr<pbr::TranslatorManager> translator_;
    // Mirrors of the engine's own STATE (plain strings: bridge.h stays free of the pipe types).
    std::wstring trMode_   = L"auto";            // auto | a2b | b2a  (a DIRECTION over the pair)
    std::wstring trLangA_  = L"en";              // the pair; TrPipe's WireState defaults (a truthful answer before
    std::wstring trLangB_  = L"zh";              // the first STATE), then engine.txt, then the engine's echo
    std::wstring trPreset_ = L"auto";            // what is RUNNING (presetReq = what the user asked for)
    std::wstring trStatus_ = L"loading";         // loading | listening | transcribing | analyzing | error
    std::wstring trAsrModel_, trMtModel_, trErr_;
    bool         trGpu_ = false;
    bool         trRunning_ = false;
    std::wstring lastTranslatorStateJson_;       // diffed

    // ---- FILE MODE (wave WC) + THE PLAYER (wave WE) + the film's own tracks (wave WH) --------------------------
    // THE STATE MACHINE IS TrPlayer.h (pbplayer::Player); PlayerHost (bridge.cpp) is its hands -- the pipe, the disk,
    // the window's mappings and reloads, the posts. What stays HERE is what needs Windows UI or a thread: the dialogs,
    // the drop's File objects, the folder scans, the subtitle files (SubtitleReader.h), the export. UI THREAD ONLY.
    struct PlayerHost;
    std::unique_ptr<PlayerHost>       playerHost_;
    std::unique_ptr<pbplayer::Player> player_;
    struct ScanBox;                                    // folder scans: worker -> UI thread
    std::shared_ptr<ScanBox>          scanBox_;
    // WH: probes and embedded-subtitle extractions (MediaTracks.h) run on WORKER threads; their answers wait here.
    struct TrackBox;
    std::shared_ptr<TrackBox>         trackBox_;
    void drainPlayerTracks();
    std::map<uint64_t, std::wstring>  extSubPaths_;    // subtitle files loaded, by id (paths never sent)
    // WG: the subtitle FILE loaded for the current item, EDITABLE (the editor's second target).
    struct ExtDoc;
    std::unique_ptr<ExtDoc> extDoc_;
    void handleSubEdit(const std::wstring& json);
    bool extDocEdit(uint64_t id, const std::vector<uint64_t>& removeIds, std::vector<TrExtCue>& adds,
                    std::vector<TrExtCue>& added, bool restore, std::wstring& err);
    bool saveExtDoc();
    void dropExtDoc();                                 // written first; a loss is SAID (review of WG #8)
    void tickExtDoc();
    void handleSubSaveAs(uint64_t id);
    void handleSetDefaultPlayer();                     // WG: "Set as default player..." (TrAssoc.h, then Settings)
    std::vector<std::wstring> extDocChunks();
    std::set<std::wstring>            trMappedHosts_;  // the media hosts mapped on the view right now
    HWND         trHwnd_ = nullptr;       // the window (dialog owner)
    bool         trDialogBusy_ = false;   // a file dialog is up: a second one is refused
    // shown / full screen / the one-shot "show the File tab", and whether the page said `ready` since its last
    // (re)load: TrWindowState.h (review of WE, C1)
    pbplayer::WindowState trWin_;
    std::wstring trLastExport_;           // the last file exported (for "Show in folder")
    bool         trProxyPurged_ = false;  // stale proxy WAVs of an earlier run deleted once
    void handlePlayerOpen(const std::wstring& what, bool play, const std::wstring& quality);
    void handleTranslatorFileDrop(const std::vector<std::wstring>& paths, const std::wstring& quality,
                                  const std::wstring& target);
    void handleTranslatorFileExport(const std::wstring& format, const std::wstring& which);
    void handleTranslatorFileReveal(const std::wstring& what);
    void handlePlayerSubLoad();                         // the subtitle Open dialog
    void loadExtSub(const std::wstring& path, uint64_t id, unsigned codepage);
    void startPlayerScan(std::vector<std::wstring> folders, std::vector<std::wstring> files,
                         std::vector<std::wstring> subs, bool play, bool force);
    void drainPlayerScans();
    void mapPlayerFolders(const std::vector<std::wstring>& folders, std::vector<std::wstring>& failed);
    void mapTranslatorProxyFolder();                    // once per view, before its first navigation
    std::wstring writeTranslatorSrt(const pbplayer::CueSet& set, const std::string& lang,
                                    const std::vector<pbt::trpipe::WireCue>& cues,
                                    const std::string& fromKey, uint32_t fromOrd,     // WH (D6): the record they come from
                                    std::wstring& note, uint64_t& size, uint64_t& mtime);   // WG: one language
    std::vector<std::wstring> pickMediaFiles(HWND owner, bool folder);
    std::wstring pickSubtitleFile(HWND owner);
    HWND         dialogOwner() const;                   // the window
    void         postToTranslatorView(const std::wstring& json);   // UI thread; the view
    void         postTranslatorFileNotice(const std::wstring& text, bool info = false);
    void         postTranslatorWindowState();

    // Off-UI-thread posts go through a SHARED sink, not through `this` (H1 fix, audit 2026-10-01): it owns the CS,
    // the pending queue and the target HWND, and outlives the Bridge while any thread holds a ref. Kept as is from
    // AudioPB -- no SuVtitle thread posts JSON off the UI thread today (the workers post a wake instead), and this
    // keeps postToWeb() safe from anywhere the day one does.
    struct WebPostSink {
        CRITICAL_SECTION cs;
        std::wstring     pending;          // guarded by cs; drained on the UI thread
        HWND             hwnd = nullptr;   // kWmBridge target; nulled when the Bridge tears down
        bool             live = false;     // false once the Bridge is gone
        WebPostSink()  { ::InitializeCriticalSection(&cs); }
        ~WebPostSink() { ::DeleteCriticalSection(&cs); }
        WebPostSink(const WebPostSink&) = delete;
        WebPostSink& operator=(const WebPostSink&) = delete;
        void enqueue(const std::wstring& j) {
            HWND h = nullptr; bool ok = false;
            ::EnterCriticalSection(&cs);
            if (live) { if (!pending.empty()) pending += L'\n'; pending += j; h = hwnd; ok = true; }
            ::LeaveCriticalSection(&cs);
            if (ok && h) ::PostMessageW(h, kWmBridge, 0, 0);
        }
    };
    std::shared_ptr<WebPostSink> sink_;
};

} // namespace patchbus
