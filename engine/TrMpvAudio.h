// =============================================================================
// SuVtitle - engine/TrMpvAudio.h   (1.1.0, wave N3: the analysis hears a file through the PLAYER's own decoder)
//
// SuVtitle.exe plays every file through libmpv (app/MpvPlayer.cpp, docs NATIVE_CONTRACT). The FILE job's sound now
// comes from the same library, HEADLESS -- no window, no sound device: mpv's own "pcm" audio output writes the
// decoded sound, 16 kHz / mono / float, into a named pipe this engine reads, as fast as it decodes (measured 400-1500x
// real time). So every file the player plays can be analysed (FLV, which Media Foundation cannot open at all;
// DTS / TrueHD / E-AC-3 / Vorbis / Opus wherever they are), and the subtitles land on the PLAYER's timeline.
//
// THE TIMELINE (measured on files built with an offset, tests/cpp/mpvaudio): mpv's time-pos IS the player's clock (the
// page draws the subtitles from it). A session opens PAUSED with an exact seek (`start=S`, hr-seek); at its first
// playback-restart mpv has written nothing yet (measured: 0 bytes after 100 ms paused) and time-pos is the presentation
// time of the first sample it WILL write -- MP4 edit lists (an empty edit = audio 1.5 s late), MPEG-TS / MKV start times
// (mpv rebases a file starting at 10 s to 0, as the player shows it), AAC priming, a target inside or before the audio.
// Facts::firstPts carries it; the reader (TrMedia.cpp) pads or trims so that frame N of its stream is time-pos N/16000.
// mpv does NOT pad a target before the audio's first sample (measured: start=1.0 on audio starting at 1.478 -> the first
// sample is at 1.478), and its exact seek lands within ONE sample of the target (2.999938 for 3.0): both are what
// firstPts says, so both are put right by the caller.
// AFTER the first sample (02/11): mpv writes the sound continuously while its clock follows the timestamps, so the
// session's filter chain (TrMpvAudio.cpp kAudioChain) fills a forward timestamp GAP of 0.1 to 30 s with silence and counts
// on across a reset (back, or forward by more than 30 s) -- Media Foundation's own rule -- and pins the output format (a
// decoded-format change would otherwise re-create mpv's output, which re-opens the pipe and ends the session).
//
// THE SOUND TRACK: `ord` = the 0-based place among the file's sound tracks in container order -- the SAME ord as
// MediaTracks / TrPipe -- mapped exactly as the player maps it (MpvPlayer.cpp: the ord-th id, in increasing order, of the
// non-external audio tracks mpv lists). -1 = mpv's OWN choice (aid=auto): what the player plays when the page names no
// track (an AVI, a TS...: MediaTracks lists no tracks there), so the analysis hears what the viewer hears.
//
// THE PIPE: created here (first instance, local clients only, inbound), named per session; mpv opens it as a file. The
// client is checked to be THIS process (mpv runs in it). Backpressure is the pipe itself: mpv's writer blocks when the
// reader does not read, so it decodes at most a few seconds ahead of the job.
// /!\ THE ONE RULE (measured: a synchronous mpv call made while the pipe was full never returned): while mpv may write,
// this code makes NO blocking mpv call -- only mpv_wait_event(0). The synchronous calls happen while the session is
// PAUSED (it writes nothing), and the teardown closes the pipe FIRST (mpv's blocked write fails at once) then destroys.
//
// THE VULKAN LOADER (1.1.0, the user's requirement: "n'importe quel ordinateur ... sans s'appuyer sur une carte
// graphique"). libmpv-2.dll imports vulkan-1.dll HARD (so does whisper.cpp's ggml-vulkan.dll), and a PC whose graphics
// driver never installed a Vulkan loader cannot even load them. The bundle ships Khronos' loader (LunarG's VulkanRT
// redistributable) in its OWN folder, <app>\mpv\vkfallback\, never next to libmpv: a PC WITH a loader in System32 keeps
// using its own (it matches its driver). Here: preloadVulkanFallback() loads the bundled one BY FULL PATH, before
// libmpv, ONLY when System32 has none (the loader then binds libmpv's import to the module already loaded); and the
// child servers get the folder appended to their PATH (environmentWithVulkanFallback: PATH comes after System32 in the
// DLL search order, so it is only ever the last resort). A loader with no driver behind it simply lists no device:
// libmpv plays with gpu-api=d3d11, the servers run on the CPU (measured with VK_LOADER_DRIVERS_DISABLE=*, PROGRESS_N3).
// ASCII only (no /utf-8 in this build).
// =============================================================================
#pragma once

#include <windows.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pbt {

// ---- libmpv, loaded at run time -----------------------------------------------------------------------------------
// Where, in order: <exe dir>\mpv\libmpv-2.dll (the installed layout, and a dev build's copy: app/CMakeLists.txt), then
// <exe dir>\libmpv-2.dll, then the folder the environment variable SUVTITLE_MPV_DIR names (a dev run of an engine built
// elsewhere) -- the app's own order (MpvPlayer::defaultDllDir), plus the dev variable. Loaded once per process (never
// unloaded: mpv's threads may outlive a handle), the Vulkan fallback first. false = unusable; `why` = one sentence.
bool mpvAudioAvailable(std::string& why);
// The folder libmpv-2.dll is (or would be) loaded from; "" = none holds it.
std::wstring mpvAudioDllDir();
// TEST ONLY (the smoke): look for libmpv-2.dll in `dir` alone ("" = the real order). Before the first load only.
void setMpvAudioDllDirForTest(const std::wstring& dir);

// ---- the Vulkan loader fallback (see the header) --------------------------------------------------------------------
constexpr wchar_t kVulkanFallbackSubdir[] = L"vkfallback";
// Does Windows' System32 hold vulkan-1.dll (a graphics driver installed the loader)?
bool systemVulkanLoaderPresent();
// <mpv dir>\vkfallback when it holds vulkan-1.dll, else "" (the mpv dir as mpvAudioDllDir() finds it, or `mpvDir`).
std::wstring vulkanFallbackDir(const std::wstring& mpvDir = std::wstring());
// Loads <mpvDir>\vkfallback\vulkan-1.dll by full path when System32 has no loader (else does nothing). Returns what it
// did, for the log ("system loader", "bundled loader loaded", "no loader at all: ...").
std::string preloadVulkanFallback(const std::wstring& mpvDir);
// A copy of this process' environment block with the fallback folder APPENDED to PATH (CreateProcessW with
// CREATE_UNICODE_ENVIRONMENT); empty = no fallback folder: inherit the environment as it is.
std::vector<wchar_t> environmentWithVulkanFallback();
// TEST ONLY: -1 = ask System32 (the default), 0 = pretend it has no loader, 1 = pretend it has one.
void setSystemVulkanLoaderForTest(int present);

// ---- one decode session -----------------------------------------------------------------------------------------------
class MpvPcm {
public:
    static constexpr uint32_t kRate = 16000;

    struct Facts {
        double      firstPts = 0.0;      // the player's time-pos of the first sample this session delivers
        double      startSec = 0.0;      // what was asked (start=)
        double      durationSec = 0.0;   // mpv's `duration` (0 = unknown)
        bool        hasVideo = false;    // a moving picture (cover art / a still image does not count)
        bool        endedAtOnce = false; // the start lies past the end of the sound: nothing to deliver (eof)
        int         audioTracks = 0;     // the file's sound tracks (external ones excluded: none are loaded)
        int         ord = -1;            // the one decoded, in container order
        bool        defaultPick = false; // asked for none: mpv's own choice (the player's then)
        int64_t     aid = -1;            // mpv's id of it
        uint32_t    srcId = 0;           // its container id (Matroska TrackNumber, ISO-BMFF track_ID...), 0 = unknown
        uint32_t    rate = 0, channels = 0;   // as stored
        std::string codec;               // a plain name ("AAC", "DTS", "Dolby Digital Plus (E-AC-3)"...)
        std::string lang, title;
        uint64_t    openMs = 0;          // the session's opening, loadfile to the paused restart
    };

    MpvPcm();
    ~MpvPcm();
    MpvPcm(const MpvPcm&) = delete;
    MpvPcm& operator=(const MpvPcm&) = delete;

    // Opens `path` PAUSED at `startSec` (exact), the sound track `ord` (-1 = mpv's choice), learns where its first sample
    // lies, then lets it run. false = an error, `err` one sentence for the user -- or `abort` said stop (err empty).
    bool open(const std::wstring& path, double startSec, int ord, std::string& err,
              const std::function<bool()>& abort = {});
    // Appends up to `maxFrames` frames, waiting for them (fewer only at the end). false = a decode error (`err`).
    bool read(std::vector<float>& out, size_t maxFrames, std::string& err);
    bool eof() const;
    void close();
    const Facts& facts() const;
    // mpv's last warnings / errors (a few lines), for the log of a failure.
    std::string lastLog() const;
    // What kind of failure the last open() was: the reader falls back to Media Foundation on a FILE-level one only (a
    // track-level refusal is final: the player has no other decoder either).
    enum class Fail { None, NoLibrary, File, Track, Aborted };
    Fail lastFail() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    Fail fail_ = Fail::None;
};

} // namespace pbt
