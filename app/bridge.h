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
// SUVTITLE LIVE (1.1.0, 2026-10-25 -- docs/LIVE_PLAN.md sections 3.3 / 4.3): the LIVE translator came back from AudioPB
// 1.1.0 (commit 50f79d6, its bridge's translator half), in a SECOND window -- the Live window (main.cpp, live.html) --
// and the bullets above change with it: the player's view may be HIDDEN while the Live window is open (the app quits
// with its LAST window; playerWindowShown), and the engine starts at the FIRST view's attach. The Live view is a second
// listener with its OWN handler (handleLiveMessage: what it says is about the LIVE scope -- its pair, its Clear, its
// strips, its Listen) and its own posts (postToLive: LINE / PART / END, the levels, the sources, the live
// translatorState). The live session's journal, Word export and replay are 1.1.0's, renamed. Every live member below
// sits in the blocks marked "SUVTITLE LIVE".
// FIX ROUND (2026-10-26, docs/REVIEW_RV-LB.md + LIVE_PLAN.md "ADDENDUM FIX ROUND"): the session survives a restart until
// Clear (app.txt LIVESESSION: replay + export of the whole file), a refused LIVE drops the wish and says why, voices are
// keyed by the engine incarnation (a respawn never hands someone else's colour on), the picker's list is asked for only
// while it is open, journal failures are said once and the lines owed are written later, a trim's LIVEIN save is
// debounced, the Export answers when a dialog is open (and a TEST INSTANCE may answer it without the OS dialog: R5), a
// key with '|' is refused, and liveDraining() lets the last window wait for the engine's drain.
// THE UPDATE BANNER (2026-11-03): AudioPB's update.check / update.status / update.open came back (they were among what the
// split left behind, above) with the banner's own three (update.later, update.skip, update.enable) -- bridge.cpp, the
// section of that name; the network half is NextaleUpdate.h.
//
// C++17: std::thread + std::atomic. UI THREAD ONLY unless a member says otherwise.
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <deque>
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
namespace pbt { namespace trpipe { struct WireCue; struct WireProxy; } }   // WG: a language's .srt is written from a cue list
namespace pbtrs { struct TranscriptOut; }   // U3: the transcript's Word file, as the core describes it (TrTranscript.h)
namespace suvmpv { class MpvPlayer; struct MpvEvent; }   // 1.1.0: the native video engine (MpvPlayer.h, N1's)

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

    // ---- THE NATIVE VIDEO ENGINE (1.1.0, docs/NATIVE_CONTRACT.md sections 2-4 and 6) ------------------------------------
    // mpv plays every file in a child window UNDER the page; the page draws the interface over it and plays it through the
    // native messages (nativeRect / nativeCmd / nativeCursor in, nativeInfo / nativeState / nativeError out). main.cpp owns
    // the MpvPlayer and hands it over HERE once load() + attach() succeeded -- or nullptr + why when they did not (the page
    // then plays everything with its own <video>); callable again. The bridge installs the event sink. main.cpp calls
    // attachNative(nullptr) BEFORE it shuts the MpvPlayer down (or shuts it down after the Bridge's shutdown()).
    void attachNative(suvmpv::MpvPlayer* player, const std::string& why = std::string());
    // kWmTranslatorWindow, the page's cursor safety net (N1): wParam 5, lParam 1 = the page LEFT full screen's idle state
    // (show the cursor), 0 = it entered it. (0..2 are the player's codes, 3 and 4 the Live window's.)
    static constexpr WPARAM kWinNativeCursor = 5;

    static constexpr UINT kWmBridge = WM_APP + 0x51;
    void setHostWindow(HWND hwnd) {
        hostWnd_ = hwnd;
        if (sink_) { ::EnterCriticalSection(&sink_->cs); sink_->hwnd = hwnd; ::LeaveCriticalSection(&sink_->cs); }
    }

    // ---- SUVTITLE LIVE (1.1.0): the windows' side ---------------------------------------------------------------
    // The Live window's view joins / leaves (main.cpp, as AudioPB's translator window did: attachTranslatorView). Its
    // HWND owns the dialog it asks for (the Export's Save dialog: an always-on-top window must not hide its own dialog).
    void attachLiveView(ICoreWebView2* webview, HWND hwnd);
    void detachLiveView();
    // The player's view went away while the app lives on (the browser process died: main.cpp rebuilds both views and
    // attaches again). Its mappings die with it; the player keeps its state.
    void detachView();
    // RV4-7 for the Live window: its REAL always-on-top (WS_EX_TOPMOST read back), told to its page on every change.
    void liveTopmostChanged(bool on);
    // The Live window was CLOSED (hidden, never destroyed): listening stops (the user, 25/10: "fermer le Live = arreter
    // l'ecoute"); the session and its journal stay.
    void liveWindowClosed();
    // main.cpp HID or SHOWED the player's window (the last-window rule: the player is hidden, not closed, while the Live
    // window is open). Hidden = its page pauses (translatorWindow{shown:false}) and the pre-analysis stops.
    void playerWindowShown(bool on);
    // kWmTranslatorWindow, the Live window's codes (0..2 above are the player's): 3 = open / bring up the Live window,
    // 4 = the Live window's always-on-top from lParam (0/1).
    static constexpr WPARAM kWinOpenLive = 3;
    static constexpr WPARAM kWinLiveTopmost = 4;
    // Fix round (LIVE_PLAN "ADDENDUM FIX ROUND", RVA-1): the engine still says it is live -- listening, or FINISHING the
    // sentences it had queued after a LIVE 0 (STATE live stays 1 until they are done). main.cpp's last window waits for
    // it (15 s at most) before the engine is told to QUIT, which would drop them.
    bool liveDraining() const { return trLive_; }

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
    // THE FREEZE WATCH (25/10): a freeze the page saw of itself -> one line of %LOCALAPPDATA%\SuVtitle\player.log
    void handlePlayerDiag(const std::wstring& json);
    ULONGLONG    diagMinute_ = 0;         // the current minute of player.log's cap (GetTickCount64)
    int          diagLines_ = 0;          // lines written in it
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
    // U3 (02/11/2026, the TRANSCRIPTION): the transcript's Word file by the core's rule (naming, the registry, Documents'
    // fallback), and the tab's Save .docx... (the dialog; default = the file written while still ours)
    std::wstring writeTranscriptDocx(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& note,
                                     uint64_t& size, uint64_t& mtime);
    // U2b (G2, the user's answer 02/11): a rename's Word file rewritten IN PLACE (t.prefer, only while still ours; false + `why`
    // "open in another program", or false + "" when it is not ours any more)
    bool rewriteTranscriptDocx(const pbplayer::CueSet& set, const pbtrs::TranscriptOut& t, std::wstring& why,
                               uint64_t& size, uint64_t& mtime);
    void handleTranscriptSave();
    std::vector<std::wstring> pickMediaFiles(HWND owner, bool folder);
    std::wstring pickSubtitleFile(HWND owner);
    HWND         dialogOwner() const;                   // the window
    void         postToTranslatorView(const std::wstring& json);   // UI thread; the view
    void         postTranslatorFileNotice(const std::wstring& text, bool info = false);
    void         postTranslatorWindowState();

    // ---- U2 (02/11/2026): THE PLAYER'S OWN MODELS ("Import model...") -- app.txt MODEL / MODELUSE (AppConfig.h) ------
    // The files are REFERENCED IN PLACE; the page names one by an id minted here (0 = SuVtitle's default), never by a path.
    struct ModelsState;                                 // the lists and the choices (bridge.cpp)
    std::unique_ptr<ModelsState> models_;
    void loadModels();                                  // app.txt -> the lists -> the core (construction: before the queue)
    void saveModels();                                  // the lists -> app.txt (the AV-safe write, only when it changes)
    void applyModels();                                 // the EFFECTIVE choice (a chosen file that is gone = the default) -> core
    void postModels();                                  // translatorModels{asr, mt} to the player's page
    void handleModelImport();                           // translatorModelImport{}: the dialog, the header decides the kind
    void handleModelPick(const std::wstring& kind, long long id);   // translatorModelPick{kind, id}
    // U2b (02/11/2026): translatorModelRemove{kind, id, recycle} -- the LIST's own path (never the page's), out of app.txt; the
    // file to the Recycle Bin when asked (IFileOperation owned by the window: never a permanent delete of ours)
    void handleModelRemove(const std::wstring& kind, long long id, bool recycle);
    void onModelFailed(const std::string& kind, const std::string& path, const std::wstring& sentence, bool say,
                       const std::wstring& redone);
    std::wstring pickModelFile(HWND owner);

    // ---- THE NATIVE VIDEO ENGINE (1.1.0): the bridge's half (bridge.cpp, section "THE NATIVE VIDEO ENGINE") ----------
    // gen = the core's Session::seq (the item's OPENING). The bridge OPENS mpv itself on the current item (syncNative,
    // after every entry point that may load or close one), never from a path the page sends; the page plays it.
    void onNativeEvent(const suvmpv::MpvEvent& e);
    void syncNative();                                  // the core's current item -> mpv (open / stop), once per opening
    void handleNativeMessage(const std::wstring& type, const std::wstring& json);   // nativeRect / nativeCmd / nativeCursor
    void nativeFallBack(uint64_t seq, const std::wstring& text, bool tellPage);    // this opening -> the page's <video>
    void postNativeInfo(bool force);                    // nativeInfo{available, why}: on change, and on every `ready`
    void replayNative();                                // `ready`: the info, mpv's state, a fallback the old page knew of
    void postNativeState(bool now, bool force);         // nativeState from MpvPlayer::state(), <= 40 Hz unless `now`
    bool nativeUsable() const;                          // handed over and running
    bool nativeWanted() const;                          // the current item plays (or is about to play) in mpv
    void detachNative();                                // the sink taken back, mpv stopped (shutdown, destructor)
    suvmpv::MpvPlayer* native_ = nullptr;   // main.cpp's (not owned); UI thread only
    bool         nativeKnown_ = false;      // attachNative was called (before: unknown -- nothing falls back yet)
    std::string  nativeWhy_;                // why not, as main.cpp said it
    bool         nativeInfoSent_ = false, nativeInfoAvail_ = false;   // what the page was last told
    uint64_t     nativeSeq_ = 0;            // the opening mpv holds (0 = none)
    uint64_t     nativeFellBack_ = 0;       // the opening the page's own <video> plays instead
    uint64_t     nativeStartMs_ = 0;        // where it was opened (the state before mpv's first word)
    bool         nativeStarted_ = false;    // fix round (L7): mpv's Start of this opening came (its I/O wait is over)
    bool         nativeLoaded_ = false;     // mpv said it has the file (Loaded / Duration / Time / VideoSize / Restart)
    bool         nativeTracksSeen_ = false; // fix round (L9): its Tracks came -- MpvState::hasVideo is the picture's answer
    bool         nativeRestarted_ = false;  // fix round (M2): its first Restart came -- `seekable` is mpv's real answer then
    bool         nativeVideoKnown_ = false; // whether it shows a picture is known (Tracks; before N1's fill: the first Restart)
    bool         nativeDirty_ = false;      // a state held back by the rate limit (flushed by the next event or uiTick)
    unsigned long long nativePostedAt_ = 0; // when the last state went (steady clock, ms)
    std::wstring nativeLastJson_;
    std::string  nativeTracksSig_;          // fix round (L6): the sound-track list last offered to the core (its signature)
    void offerNativeTracks();               // fix round (L6): mpv's audioList -> the core, where MediaTracks has none
    void resetNativeItem();                 // what is known of the opening mpv holds: nothing (a new one, or none)
    // (fix round L2/L3: the SOUND COPY is no longer asked while mpv plays at all -- PlayerHost::mpvPlays, TrPlayer.h -- so
    //  the bridge holds none: an item that falls back is handed back to the core, mpvDropped, which asks for it then)

    // ---- SUVTITLE LIVE (1.1.0): the live translator (AudioPB 1.1.0's translator half, docs/LIVE_PLAN.md 3.3/3.4/4.3) ----
    void startEngine();                                 // once, at the FIRST view's attach (either window's)
    void handleLiveMessage(const std::wstring& json);  // the Live view's messages: the LIVE scope
    void handleLiveCtl(const std::wstring& mode, const std::wstring& preset, bool clear,
                       const std::wstring& langA, const std::wstring& langB);
    void handleLiveConfig(const std::wstring& json);   // liveConfig{inputs}: checked, persisted, applied if listening
    void handleLiveListen(bool on);                     // liveListen{on}: LIVE 1 <strips> / LIVE 0
    void postToLive(const std::wstring& json);          // the Live view only (UI thread)
    void postLiveWarn(const std::wstring& text);        // {type:"warn", text} to the Live view
    void postLiveWindowState();                         // translatorWindow{topmost} to the Live view
    void sendLiveSources();                             // liveSources{gen, list} (the last complete list)
    std::wstring liveJson() const;                      // translatorState.live = {on, wanted, inputs:[...]}
    void sendLiveState(bool force);                     // the Live view's translatorState (the LIVE pair), diffed
    void updateLiveGate();                              // the player's gate = wanted || the engine's echo
    void endLiveSession();                              // a Clear: the log, the session file, colours, voices
    void loadLiveConfig();                              // app.txt LIVEIN, engine.txt's live pair (READ only), once
    void saveLiveConfig();                              // app.txt LIVEIN (the AV-safe write, only when it changes)
    std::wstring stripLabelFor(const std::wstring& lane) const;  // a speaker's name: its LANE's strip label, else...
    int          translatorSpeakerColor(const std::wstring& key);   // stable, by first appearance (1.1.0)
    // W5 (1.1.0) -- the durable session log, and the Word document made from it: the journal is written as the meeting
    // happens (the replay is capped at 200 lines and a window can be closed), the export reads it back.
    void appendTranslatorSession(const std::wstring& lineJson);
    void handleTranslatorExport(const std::wstring& format);
    void postTranslatorExported(bool ok, const std::wstring& pathOrErr, long long lines);
    void replayTranslatorLog();                         // a (re)loaded Live page gets the scrollback back
    // ---- fix round (2026-10-26, docs/REVIEW_RV-LB.md) ----
    void reattachLiveSession();                         // RVB-1: app.txt LIVESESSION -> the journal, its count, its tail
    void setLiveSessionTag(const std::string& name);    // RVB-1: app.txt LIVESESSION <name> ("" = none: a Clear)
    bool writeJournalPending();                         // RVB-12: what the journal is owed, written (true = all landed)
    void journalFailed(DWORD err);                      // RVB-12: said once per run of failures (warn + live.log)
    void flushLiveConfig();                             // RVB-13: a debounced LIVEIN save, written now if one is due

    ICoreWebView2* liveView_ = nullptr;      // the Live window's view (not owned); UI-thread only
    int64_t        liveViewToken_ = 0;
    HWND           liveHwnd_ = nullptr;      // the Live window (its dialogs' owner)
    bool           livePageReady_ = false;   // the Live page said `ready` since its last (re)load
    bool           liveTopmost_ = false;     // its REAL WS_EX_TOPMOST (main.cpp reads it back)
    bool           engineStarted_ = false;
    // The LIVE pair: mirrors of the engine's STATE liveMode/liveLangA/liveLangB (the engine owns them: engine.txt), seeded
    // from engine.txt (READ only) or else the player's pair until the first echo. Never the player's pair (D-L4).
    std::wstring   liveMode_ = L"auto", liveLangA_, liveLangB_;
    bool           trLive_ = false;          // the engine's echo: the live path runs (STATE live)
    bool           liveWanted_ = false;      // the app's wish (Listen) -- never at launch (D-L8)
    bool           liveGate_ = false;        // what the player was last told (setLive)
    // The voiceprint stage, as the engine reports it (1.1.0): off = no diarization payload, speakers named by strip.
    bool           trSpkOn_ = false;
    std::wstring   trSpkModel_;
    int            trSpkCount_ = 0;
    // The strips (app.txt LIVEIN, then the page's liveConfig), with each one's last INPUT state (its DEVICE's). A strip's
    // name on the wire is trpipe::liveLaneId(key, chan) (TrPipe.h LANES), computed where needed, never stored.
    struct LiveStrip { std::string key, label; uint32_t chan = 0; double gainDb = 0; std::string state, why; };
    std::vector<LiveStrip> liveStrips_;
    bool           liveConfigLoaded_ = false;
    // The picker's last complete list (the engine's SRC lines): its JSON for the page, its keys (a strip's key from the
    // page must be one of them, or a strip's already: no string of the page's own reaches the engine), its labels (a
    // strip with no name of its own is named after its source).
    std::wstring   liveSrcList_;             // the `list` array, "" = no list yet
    uint32_t       liveSrcGen_ = 0;          // OURS for the page: monotonic across engine incarnations (theirs restart)
    std::set<std::string> liveSrcKeys_;
    std::map<std::string, std::string> liveSrcLabels_;
    std::wstring   lastLiveStateJson_;       // diffed, like lastTranslatorStateJson_
    // The session (1.1.0 W5): the journal file ("" until its FIRST finished line: an idle app leaves no empty file),
    // its line count (rides translatorState as `sessionLines`), the scrollback the Live page replays (FINISHED lines only,
    // verbatim: never a PART, never an END), the speakers' colours (order of appearance, never reassigned, reset with the
    // session) and each strip's voices in order of first appearance (the " . 2" of the label rule).
    std::wstring   trSessionPath_;
    long long      trSessionLines_ = 0;
    static constexpr size_t kTranslatorLogMax = 200;
    std::deque<std::wstring> translatorLog_;
    static constexpr int kSpeakerColors = 6;  // the Live page's palette (1.1.0's bridge said 8 against the page's 6)
    std::map<std::wstring, int> spkColor_;
    int            spkNextColor_ = 0;
    std::map<std::wstring, std::vector<std::wstring>> spkVoices_;
    HWND           exportOwner_ = nullptr;   // the window whose message asked for the export (the Live window)
    // Fix round (2026-10-26): RVB-6 -- this RUN's nonce, which with the engine's incarnation (Msg::gen) names a voice: each
    // engine names its clusters from S1 again, and a session reattached after a restart holds the older runs' voices;
    // RVB-12 -- the bytes the journal is owed (a write that failed: tried again with the next line, bounded), how many
    // of its newlines are not records (the one that ends a torn last line found at the reattach), whether the failure
    // was said; RVB-13 -- when a trim's debounced LIVEIN save is due (0 = none).
    std::wstring   spkRun_;
    std::string    trJournalOwed_;
    size_t         trJournalSeps_ = 0;
    bool           trJournalWarned_ = false;
    ULONGLONG      liveCfgSaveDue_ = 0;

    // ---- THE UPDATE BANNER (2026-11-03, bridge.cpp section of that name) --------------------------------------------
    // The site's manifest is read ONCE per start (the page's first update.check, after its `ready`), OFF the UI thread: the
    // worker holds the box and the window's HWND, never `this` (H1), and rings kWmTranslatorPoll -- pumpTranslator drains it
    // (drainUpdate; uiTick is the safety net). app.txt UPDATECHECK / UPDATESKIP (AppConfig.h) are the opt-out and the skipped
    // version -- which the Updates dialog still names, and can offer again (update.unskip). "Later" lasts for the run: a page
    // reloaded for a new folder asks again and is told it is put away.
    // update.open opens SuVtitle's page, NextaleUpdate.h pageUrl(the offered version, this version): the app's own constant,
    // nothing the page or the manifest names.
    struct UpdBox;                                      // the worker -> the UI thread (bridge.cpp)
    std::shared_ptr<UpdBox> updBox_;
    bool         updEnabled_ = true;                    // UPDATECHECK (absent = on)
    std::string  updSkip_;                              // UPDATESKIP ("" = none)
    bool         updStarted_ = false;                   // the check ran / runs in this run (once per start)
    bool         updDone_ = false;                      // its answer was drained
    bool         updLater_ = false;                     // "Later": put away until the next start
    std::string  updVersion_, updNotes_;                // a newer version's (updVersion_ "" = none known)
    // (review 03/11, N3) the last update.open answered: when, and its update.opened -- a repeat within 2 s (a double-click; the
    // banner's button then the dialog's) gets that answer again and opens nothing a second time
    ULONGLONG    updOpenAt_ = 0;
    std::wstring updOpenJson_;
    void loadUpdatePrefs();                             // app.txt -> updEnabled_ / updSkip_ (construction)
    void saveUpdatePrefs();                             // -> app.txt (the AV-safe write, only when it changes)
    void startUpdateCheck();                            // once, if enabled (a TEST INSTANCE: a loopback test server only)
    void drainUpdate();                                 // the worker's answer -> update.status
    void postUpdateStatus();                            // update.status{enabled, available, version, notes, later, skipped...}
    void openUpdatePage();                              // update.open{}: SuVtitle's page (pageUrl), nothing else

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
