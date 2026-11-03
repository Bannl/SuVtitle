// =============================================================================
// SuVtitle - engine/TrCaptureApi.h   (the LIVE capture: what the engine core and the capture hub promise each other)
//
// SuVtitle 1.1.0 (2026-10-25). AudioPB's live translator read its audio from AudioPB's own routing; SuVtitle captures
// it ITSELF through Windows (the user, 24/10: "SuVtitle capture aussi seul par Windows"). This file is the seam between
// the two halves that build that path, so they can be written apart and meet without surprises:
//   * the ENGINE CORE (main_translator.cpp's live pipeline, TrAudio: the VAD, the live line, the commit, the speaker
//     attribution -- ported from AudioPB 1.1.0) consumes 16 kHz audio through ILiveSink and drives the hub;
//   * the CAPTURE HUB (TrCaptureHub.cpp and its sources: WASAPI inputs, output loopback, one application's loopback,
//     the test WAV source) produces it.
// OWNED BY THE DIRECTOR (docs/LIVE_PLAN.md): both agents build against it and neither edits it; a change one of them
// needs is asked for, and made here for both.
//
// THE SHAPE
//
//   one thread per opened DEVICE                       the hub's MIXER thread                    the engine core
//   WASAPI event -> float -> for each strip using     every ~10 ms of QPC time: per lane, take  ILiveSink::feed16k(
//   this device: channel pick (or the sum) x trim ->  the frames due from its FIFO (zeros when     mix, perInput)
//   NaN/inf -> 0, clamp +-4 -> Decimator -> that      it runs dry, one sample slipped when it   -> the VAD, exactly as
//   strip's 16 kHz FIFO                               drifts), sum them = the mix                AudioPB's audio thread
//
//   * One DEVICE opened once, whatever the number of strips on it: two strips on channels 1 and 2 of one interface
//     share its thread and its clock (one interface, two people: "canal 1 = moi, canal 2 = l'invite" -- an exact attribution,
//     zero ML).
//   * The MIXER THREAD OWNS THE VAD while the live path runs, as AudioPB's audio thread did: bindInputs and feed16k are
//     only ever called on it. Every hand-off TrAudio already had (the segment queue, the live-line slot, the commit
//     request, the clear flag) stays as it was, with its own mutex.
//   * The per-input energy that names the speaker is measured on each lane's OWN contribution, after its trim and
//     before the sum -- AudioPB's arithmetic ("measured PRE-MIX, per cable k"), with "cable" read "strip".
//   * Silence is ZEROS at a steady cadence, never an absence of calls: a loopback with nothing playing sends no packet
//     at all, and the VAD must still see time pass (its hangover closes a segment on silence).
//
// THE RULES THE HUB KEEPS
//   * Never block the mixer on a device, never block a device thread on the mixer: one SPSC FIFO per strip, bounded
//     (~2 s at 16 kHz); a full FIFO drops its OLDEST audio and counts it.
//   * A lane whose device fails (unplugged, taken exclusively, refused by the privacy settings) goes SILENT and says
//     so through ILiveEvents::onInputState; the others carry on, and the VAD never stops. A device that comes back
//     (same endpoint id) is reopened on its own, with a growing back-off.
//   * stop() joins every capture thread it started; after it returns no ILiveSink call and no input/levels event runs
//     or will run (the engine then drains what the VAD already cut: LIVE 0 in TrPipe.h). The source-list watching
//     lives with the hub, not with a live run (RVA-9): onSources may still fire after stop(), until the hub is destroyed.
//   * No call into ILiveEvents while holding a lock the caller of start/stop/requestSources might wait on.
//
// CLEAN ROOM (docs/LIVE_PLAN.md): the capture is written from Microsoft's documentation, never from AudioPB's router
// code; AudioPB's routing core never enters this repository, its comments included (tests/boundary/check_boundary.js).
// =============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "TrPipe.h"        // kMaxLiveInputs, WireLiveInput: one definition of a strip, shared with the wire
#include "TrDecimator.h"   // kAsrRate

namespace pbt {

constexpr uint32_t kMaxLiveInputs = trpipe::kMaxLiveInputs;

// The mixer's block: at most this many 16 kHz frames per feed16k (10 ms). Smaller than one VAD hop (30 ms), so the
// per-hop energy snapshots keep AudioPB's granularity.
constexpr int kFeedMaxFrames = kAsrRate / 100;

// One strip of the Live window, as LIVE carries it (already validated by trpipe::unpackLive: kind, channel, gain,
// the "same key twice" rule).
using LiveInput = trpipe::WireLiveInput;

// One line of the picker, as SRC carries it (TrPipe.h: kind in|out|app, state ok|absent|busy|denied|unsupported,
// flags "default"/"comm").
struct SourceInfo {
    std::string kind;
    std::string key;
    std::string label;
    std::string state;
    uint32_t    rate = 0;
    uint32_t    ch = 0;
    std::string flags;
};

// ---------------------------------------------------------------------------
// The hub -> the VAD. Implemented by TrAudio (the engine core). Called ONLY on the hub's mixer thread.
// ---------------------------------------------------------------------------
class ILiveSink {
public:
    virtual ~ILiveSink() = default;

    // Before the first feed16k of a new input SET, and again each time the set changes: keys[k] = the LANE ID of
    // lane k (trpipe::liveLaneId: two channels of one device are two lanes; "" = an empty lane). The engine core keeps
    // AudioPB's rule that the names are frozen when a segment OPENS (a strip removed mid-sentence does not relabel the
    // speaker of that sentence).
    virtual void bindInputs(const std::string (&keys)[kMaxLiveInputs]) = 0;

    // n (1..kFeedMaxFrames) frames at 16 kHz: `mix` = the sum of the lanes; perInput[k] = lane k's own contribution
    // (the same n frames, after its trim; nullptr for an empty lane). Every sample finite and within +-4 (the hub
    // guards). Called at a steady cadence -- one block per ~10 ms of real time, or of the virtual clock in a test --
    // whether or not anyone talks.
    virtual void feed16k(const float* mix, int n, const float* const (&perInput)[kMaxLiveInputs]) = 0;
};

// ---------------------------------------------------------------------------
// The hub -> the engine's servant (which turns these into INPUT / LEVELS / SRC + SRCEND on the pipe).
// Called on the hub's own threads. Implementations ENQUEUE and return: never block, never call back into the hub.
// ---------------------------------------------------------------------------
class ILiveEvents {
public:
    virtual ~ILiveEvents() = default;

    // A live input's state CHANGED (TrPipe.h INPUT: open | absent | busy | denied | unsupported | stalled | error).
    // `why` = "" or one human sentence, e.g. "Windows blocks microphone access for desktop apps (Settings > Privacy >
    // Microphone)." Never repeated for an unchanged state.
    virtual void onInputState(const std::string& key, const std::string& state, const std::string& why) = 0;

    // ~5 Hz while the live path runs (never otherwise): each lane's PEAK over the last ~200 ms, after its trim, dBFS,
    // named by its LANE ID. (onInputState above stays keyed by the DEVICE key: its state is every one of its lanes'.)
    virtual void onLevels(const std::vector<std::pair<std::string, float>>& peakDbfs) = 0;

    // The picker's list: the answer to requestSources(), and again on its own -- coalesced, at most every ~500 ms --
    // each time the device or application list changes after the first request.
    virtual void onSources(const std::vector<SourceInfo>& list) = 0;

    // One line for the engine's own log (a device that failed and why, a format negotiated, a process re-targeted):
    // what a user's bug report needs and the pipe does not carry. Optional for an implementation.
    virtual void onLog(const std::string& line) { (void)line; }
};

// ---------------------------------------------------------------------------
// The hub. Constructed once by runHost; driven by the pipeline thread (start/stop) and the servant (requestSources).
// Every public method is safe to call from any thread, and none of them blocks for long: start/stop return once the
// sources are opened/joined (bounded by the hub's own timeouts, a few seconds at worst for a hung driver).
// ---------------------------------------------------------------------------
struct HubOptions {
    // SuVtitle.exe's pid (the engine's --parent): its process tree -- the app, the engine, WebView2's children -- is never
    // listed as an application source. 0 = unknown (only the engine's own process is hidden then).
    uint32_t appPid = 0;
    // TEST ENGINE ONLY (testEnvAllowed in main_translator.cpp: a selftest, --cpu): accept "wav:" keys.
    bool testSources = false;
    // TEST ONLY: the mixer and the WAV sources run on a VIRTUAL clock -- as fast as the CPU allows, in lockstep, with no
    // sleeping -- so a 10-minute drift run takes seconds and gives the same result every time. Never with a device.
    bool virtualClock = false;
};

class TrCaptureHub {
public:
    TrCaptureHub(ILiveEvents* events, const HubOptions& opts = HubOptions{});
    ~TrCaptureHub();   // = stop()

    TrCaptureHub(const TrCaptureHub&) = delete;
    TrCaptureHub& operator=(const TrCaptureHub&) = delete;

    // Start the capture on `inputs` (1..kMaxLiveInputs), feeding `sink` -- or REPLACE the running set: a lane whose
    // {key, chan, gainDb} did not move keeps running untouched (the label is the app's business, not the hub's).
    // Returns once every input has either opened or reported why not through onInputState; a device that fails to open
    // is NOT a failure of start (its lane stays silent and retries). false = refused outright: an empty or oversized
    // set, a "wav:" key without HubOptions::testSources, a null sink.
    bool start(const std::vector<LiveInput>& inputs, ILiveSink* sink);

    // Stop every source and the mixer, join their threads. Idempotent.
    void stop();

    bool running() const;

    // Enumerate the sources now (on the hub's own thread) and answer through ILiveEvents::onSources; from then on,
    // push a new list whenever it changes. Never blocks the caller.
    void requestSources();

    // The measured front-end delay of an input, in ms -- device period + FIFO fill + decimator group delay -- or 0 when
    // unknown. The pipeline may take it off waitMs for the dominant input (SUVTITLE_LIVE_PLAN section 4.7).
    double inputDelayMs(const std::string& laneId) const;   // laneId = LINE.spkSlot (trpipe::liveLaneId)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pbt
