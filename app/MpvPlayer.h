// =============================================================================
// SuVtitle - app/MpvPlayer.h   (the native video engine: libmpv under the page)
//
// 1.1.0 (26/10/2026). The user: "ajouter un vrai moteur video natif", "on y va pour le moteur natif, on arrete avec le
// bricolage"; and "que SuVtitle puisse etre installe sur n'importe quel ordinateur et puisse lire le plus de formats
// possible sans s'appuyer sur une carte graphique". mpv plays EVERY file; the WebView draws the interface over it (visual
// hosting, a DirectComposition target topmost on the player window); the old <video> path is the automatic fallback.
// Contract: docs/NATIVE_CONTRACT.md section 3 (+ its "Changes"). Reference: scratchpad\n0\src\n0proto.cpp (passed on his
// screen, 26/10).
//
// N1 (26/10/2026): the real thing (the N-L0 stub's API kept, extended -- every addition is in the contract's "Changes").
//   * libmpv-2.dll is loaded at RUN TIME (LoadLibraryExW + GetProcAddress, never a hard import): no DLL, or one that
//     cannot start, and available() is false with a sentence in why() -- the page then plays everything with <video>.
//   * mpv renders into a DISABLED child window of the player's window (`wid`): hit tests skip a disabled window and its
//     whole subtree, so every real mouse message over the picture reaches the PARENT (main.cpp forwards it to the page) and
//     never the window mpv creates inside ours on its own thread.
//   * mpv's events: its wakeup callback only POSTS to that child; the child drains them on the UI thread and calls the sink.
//   * The PICTURE'S PLACE is the page's: the bridge passes the stage's rectangle in CSS pixels (placeCss); this class owns
//     the pixel maths (setDpiScale = the WebView's RasterizationScale x ZoomFactor, set by main.cpp).
// FIX ROUND (26/10 evening, docs\REVIEW_RV-NH.md): every item's sound track is FILE-LOCAL (aid=auto when none is asked: a
// switch never outlives its item); a file never makes mpv follow a reference (access-references=no: no URL fetched from a
// local playlist / manifest / EDL); a seek, B jump or frame step asked while the item loads is kept and replayed once it is
// loaded; play() at the end restarts only if nothing was sought since; the track list (Tracks / audioList / hasVideo); the
// DLL search path locked (lockDllSearch, wWinMain's first call).
// UI THREAD ONLY: every call, and every event callback, happens on the window's thread. ASCII only.
// =============================================================================
#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace suvmpv {

// One event from mpv, already on the UI thread. `kind` says which fields mean something; whatever the kind, state() holds
// the WHOLE current state (the bridge builds nativeState from it).
struct MpvEvent {
    enum class Kind : uint8_t {
        Time, Duration, Pause, Eof, Speed, Volume, Track, AbLoop, VideoSize, Hwdec, Error, Idle,
        // N1 (appended -- the values above are unchanged):
        Available,   // the engine became usable / unusable: flag = available, text = why not. GLOBAL (gen ignored)
        Start,       // the item `gen` began loading (mpv's start-file): its per-item state was reset
        Loaded,      // the item's file is open, its tracks known (mpv's file-loaded); the duration follows as its own event
        Seeking,     // flag = a seek is in progress (the page freezes its interpolated clock meanwhile)
        Restart,     // playback (re)started after a seek or the load: the page's 'seeked' / 'canplay'
        Seekable,    // flag = the file can be seeked
        // fix round (26/10, appended):
        Tracks       // the item's track list changed: state().audioList / hasVideo. Sent BEFORE Loaded (so both are known
                     // at Loaded), and again whenever a stream appears later (MPEG-TS can add one mid-file)
    };
    Kind        kind = Kind::Idle;
    uint64_t    gen = 0;          // the item generation passed to open() (0 = none): a stale event is the caller's to drop
    int64_t     ms = -1;          // Time: the playhead; Duration: the length (-1 = unknown)
    bool        flag = false;     // Pause: paused; Eof: reached; Volume: muted; Hwdec: WARP in use; Idle: idle;
                                  // Available / Seeking / Seekable: as named
    double      value = 0.0;      // Speed: rate; Volume: percent (the LINEAR gain of <video>.volume x 100)
    int         ord = -1;         // Track: the sound track now heard (0-based among the sound tracks, container order)
    int64_t     abA = -1, abB = -1; // AbLoop: the loop (-1 = off)
    int         w = 0, h = 0;     // VideoSize: the picture (0 x 0 = no picture: an audio file)
    std::string text;             // Hwdec: the decoder ("d3d11va", "no"...); Error / Available: one sentence
};

// Everything known now. Per-item fields are reset at each Start -- but abA / abB and w / h MIRROR mpv's own properties (mpv's
// A-B loop outlives a file and open() clears it; mpv does not say again a picture size that did not change); pause / speed
// / volume / mute are the player's own and carry over from one item to the next (the page sets them again anyway).
struct MpvState {
    uint64_t    gen = 0;          // the item loaded in mpv now (0 = none)
    int64_t     timeMs = -1, durMs = -1;
    bool        paused = true, eof = false, seeking = false, seekable = false, idle = true;
    double      speed = 1.0;
    int         volPct = 100;     // LINEAR, as <video>.volume x 100
    bool        muted = false;
    int         ord = -1;         // the sound track heard (-1 = none / not known yet)
    int         audioTracks = 0;  // how many sound tracks the item has (container order = ord order)
    // fix round (26/10): the sound tracks as mpv lists them, in ORD order (audioList.size() == audioTracks): the track menu's
    // source for the containers MediaTracks cannot read (AVI, TS, MPG, WMV, FLV...). The strings are the FILE's (untrusted).
    struct Audio { int64_t id = -1; std::string lang, title, codec; int channels = 0; bool isDefault = false; };
    std::vector<Audio> audioList;
    bool        hasVideo = false; // the item has a picture track that is not cover art (track-list): known at Loaded
    int64_t     abA = -1, abB = -1;
    int         w = 0, h = 0;     // the picture's display size (0 x 0 = none): mpv's dwidth / dheight (not reset at Start)
    std::string hwdec;            // "d3d11va", "no"... ("" until the first frame is decoded)
    bool        warp = false;     // rendering in software (WARP / the Basic Render Driver): profile=fast is on
};

// Set once, BEFORE attach().
struct MpvOptions {
    bool         silent = false;     // ao=null: never a sound (a TEST INSTANCE: no automation may ever play out loud)
    bool         forceWarp = false;  // d3d11-warp=yes + profile=fast, as on a PC with no usable GPU (tests)
    std::wstring logPath;            // native.log ("" = none): mpv's messages + the engine's own lines
    std::vector<std::string> extra;  // "key=value" mpv options applied LAST (tests, diagnostics: ao=pcm...); never the app's
};

class MpvPlayer {
public:
    MpvPlayer();
    ~MpvPlayer();                                // shutdown() if it was not called
    MpvPlayer(const MpvPlayer&) = delete;
    MpvPlayer& operator=(const MpvPlayer&) = delete;

    static constexpr const wchar_t* kDllName = L"libmpv-2.dll";
    // Where libmpv-2.dll is looked for: <exe dir>\mpv\ (the bundle layout) if it holds one, else <exe dir>.
    static std::wstring defaultDllDir();
    // THE PROCESS'S DLL SEARCH PATH, locked (wWinMain's FIRST call, before any library is loaded by name): the exe's folder,
    // System32 and <exe dir>\mpv\ only -- never the current directory (Explorer starts SuVtitle with --play in the FILM's
    // folder, and libmpv loads DLLs by name at run time that most PCs do not have: nvcuda, d3dcompiler_43, libEGL...).
    // False if Windows refused (the search stays the default one).
    static bool lockDllSearch();

    // libmpv-2.dll from `dllDir` (LoadLibraryExW + GetProcAddress); false + a sentence in `why` when it is not usable.
    bool load(const std::wstring& dllDir, std::string& why);
    bool loaded() const;
    void setOptions(const MpvOptions& o);        // before attach()
    // the DISABLED child window (hit tests fall through to the parent) that mpv renders into (`wid`), and mpv itself,
    // created and initialised (idle: no file). Once per app; false + why() when mpv could not start.
    bool attach(HWND parent);
    bool available() const;                      // loaded, attached, initialised, not shut down
    const std::string& why() const;              // why not available ("" when it is)
    void setEventSink(std::function<void(const MpvEvent&)> sink);
    const MpvState& state() const;

    // Loads `path` PAUSED at startMs (like a <video> given a src: the caller sends play()), with the sound track `ord`
    // (-1 = mpv's default choice) -- FILE-LOCAL either way: a switch made during the item never outlives it; the item's
    // A-B loop is cleared. Events of the item carry `gen`.
    void open(const std::wstring& path, uint64_t startMs, int ord, uint64_t gen);
    void stop();                                 // the item is closed: mpv goes idle (no file)
    // at the end (eof): from the start, as <video>.play() does -- unless a seek was asked since that end (it is on its way)
    void play();
    void pause();
    // While the item LOADS (open() .. its file-loaded) mpv refuses seeks: the last one asked (a seek, setAbLoop's jump to A,
    // a frame step) is kept and done once the item is loaded (a frame step: at the item's first restart, once mpv's own
    // start seek is done -- mpv ignores a back step during it); dropped if another open() / stop() comes first.
    void seek(uint64_t ms, bool exact);
    void setSpeed(double rate);
    void setVolume(int pct);                     // LINEAR 0..100, as <video>.volume = pct / 100 (mpv's own curve is cubic)
    void setMute(bool on);
    void setAbLoop(int64_t aMs, int64_t bMs);    // -1 = off; setting B jumps to A (N0's fix)
    void selectAudio(int ord);
    void frameStep(int step);                    // +1 / -1: one frame (playback pauses)
    void place(const RECT& clientRect);          // the picture's rectangle in the parent's client pixels (empty = hidden)
    // The picture's rectangle in the PAGE's CSS pixels (nativeRect): w or h <= 0 = hidden. Multiplied by dpiScale().
    void placeCss(double x, double y, double w, double h);
    void setDpiScale(double scale);              // CSS px -> client px: RasterizationScale x ZoomFactor (main.cpp)
    double dpiScale() const;
    // Full screen in / out (main.cpp's setFullscreen, BEFORE it resizes the window): in full screen the stage is the whole
    // page, so the picture fills the client at once -- the page's next nativeRect confirms it; out, the windowed rectangle
    // comes back until the page says otherwise.
    void setFullscreenHint(bool on);
    void parentResized();                        // main.cpp's WM_SIZE: the rectangle applied again
    // mpv terminated WITHOUT blocking the UI thread on a deadlock (its window is a cross-thread child of ours: it may send us
    // messages while it goes): a worker terminates, this thread answers SENT messages meanwhile, 15 s at most. Twice = once.
    void shutdown();
    void diag(const std::string& line);          // one line in native.log (main.cpp's host events)
    HWND videoWindow() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}   // namespace suvmpv
