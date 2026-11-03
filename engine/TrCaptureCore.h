// =============================================================================
// SuVtitle - engine/TrCaptureCore.h   (the capture's building blocks: a strip's lane, its FIFO, the fill rule)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b. The parts of the capture hub (TrCaptureHub.cpp) that do not talk to
// Windows' audio stack, in a file of their own so the capture smoke (tests/cpp/capture) can drive them one by one:
//   * LaneFifo      one strip's 16 kHz FIFO between its source's thread and the mixer: bounded, one producer, one
//                   consumer, and neither side EVER waits for the other; full = the OLDEST audio goes, counted.
//   * Lane          one strip: channel pick (or the sum) x trim -> NaN/inf -> 0, clamp +-4 -> its own Decimator
//                   -> its FIFO (the producer half, on the source's thread); the mixer's fill rule -- zeros when
//                   dry, one sample slipped on drift, a resync after a long excess (the consumer half).
//   * DeviceFeed    what a source hands each packet to: the lanes bound to that source (one device, any number of
//                   strips on it), edited live by a replacement.
//   * Source        what the hub drives: a WASAPI device, an application, a test WAV file.
//   * EventGate     every ILiveEvents call goes through it, and stop() closes it: the contract's "no call after
//                   stop() returns", even for a thread stuck in a driver that the hub had to abandon.
//   * StateReporter INPUT only when the state CHANGES.
//   * SampleFormat  what WASAPI and WAV files deliver (PCM 8/16/24/32, float 32/64), decoded to float.
// Written from Microsoft's documentation and this repository's own contract (engine/TrCaptureApi.h) only -- the
// clean room of docs/LIVE_PLAN.md section 4.2.
// =============================================================================
#pragma once

#include "TrCaptureApi.h"

#include <mmreg.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbt {
namespace cap {

// ---------------------------------------------------------------------------
// The numbers of the fill rule (SUVTITLE_LIVE_PLAN section 4.2, settled here; the capture smoke measures them).
// ---------------------------------------------------------------------------
// A strip's FIFO: 2.05 s at 16 kHz. Big enough that a VAD stalled for a second (a GC-like hiccup of the engine, a
// debugger) loses nothing; a power of two so the ring index is a mask.
constexpr int kFifoCapacity = 32768;
// The fill the mixer keeps in front of it, per kind of source. An input (and the WAV test source, which imitates one)
// delivers a packet per engine period, steadily: 40 ms covers that period, the mixer's own 10 ms block and the
// scheduling jitter of two threads. A loopback (an output, an application) is delivered by the audio engine in the
// same rhythm but stops dead when nothing plays and restarts in bursts: 60 ms. Raised for a source whose packets
// are big (see Lane::target).
constexpr int kTargetInFrames   = kAsrRate * 40 / 1000;
constexpr int kTargetLoopFrames = kAsrRate * 60 / 1000;
// Drift: the fill (smoothed, measured BEFORE each block's read, so a steady lane sits exactly on its target) may
// wander this far either side of the target before ONE sample is dropped or repeated. At 500 ppm that is a slip
// every ~125 ms -- 62.5 us of audio, below anything a recogniser hears. 10 ms, not the study's first guess of 20:
// a lane on a drifting clock settles at the band's edge, and the band plus a packet's phase (<= 10 ms) must stay
// inside one VAD hop (30 ms) of a lane that does not drift -- the capture smoke's [C3]. The EMA keeps a packet
// rhythm or scheduling jitter from ever reaching the band.
constexpr int kSlipBandFrames   = kAsrRate * 10 / 1000;
// At most one slip per this many blocks: <= 50 slips/s = 3 125 ppm of correction, far above any real clock.
constexpr int kSlipEveryBlocks  = 2;
// The fill EMA, per block (~10 ms): a time constant of ~0.3 s, so a source's packet rhythm does not read as drift.
constexpr double kFillAlpha     = 1.0 / 32.0;
// A fill this far ABOVE the target for kResyncBlocks blocks running is not drift but a stall that is over (the
// consumer was held up, a driver delivered a backlog): a slip a sample at a time would take minutes to drain it,
// so the lane jumps back to its target at once (counted).
constexpr int kResyncFrames     = kAsrRate * 200 / 1000;
constexpr int kResyncBlocks     = 10;
// Every sample the hub hands on is finite and within this (TrCaptureApi.h: "the hub guards").
constexpr float kGuardLimit     = 4.0f;

inline float guardSample(float v) {
    if (!(v == v) || v > 1e30f || v < -1e30f) return 0.0f;    // NaN and +-inf (and absurd magnitudes)
    if (v > kGuardLimit)  return kGuardLimit;
    if (v < -kGuardLimit) return -kGuardLimit;
    return v;
}

inline float linFromDb(double db) { return (float)std::pow(10.0, db / 20.0); }

// dBFS of a peak (|x|), -120 for silence, one decimal is what LEVELS carries.
float peakDbfs(float peak);

// The Decimator's latency at `rate` Hz, in ms: its output for input sample t leaves once t + halfTaps input samples
// are in, with halfTaps = ceil(32 lobes / (2 x 0.85 x 8 kHz / rate)), clamped 4..512 -- TrDecimator.cpp's own
// formula (its constants are private). The capture smoke checks this against the real Decimator's impulse response
// at every rate, so a change of the director's file cannot leave this silently wrong.
double decimatorDelayMs(uint32_t rate);

// ---------------------------------------------------------------------------
// LaneFifo -- bounded SPSC ring of 16 kHz samples, drop-oldest.
//
// The producer (a source's thread) never waits: when the ring is full it moves the READ index forward itself (a
// compare-exchange: the consumer may be moving it at the same moment) and counts what it dropped. The consumer (the
// mixer) never waits either: it copies, then commits its read with a compare-exchange; if the producer dropped the
// very samples it was copying, the commit fails and it copies again from the new start. The samples are relaxed
// atomics, so even that discarded copy is not a data race.
// ---------------------------------------------------------------------------
class LaneFifo {
public:
    LaneFifo();
    LaneFifo(const LaneFifo&) = delete;
    LaneFifo& operator=(const LaneFifo&) = delete;

    // producer
    void push(const float* x, int n);
    // consumer
    int  size() const;                  // samples waiting (exact for the consumer, a snapshot for anyone else)
    int  read(float* out, int n);       // up to n, oldest first; returns how many
    int  skip(int n);                   // drop up to n of the oldest; returns how many
    // anyone
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr uint64_t kMask = (uint64_t)kFifoCapacity - 1;
    std::unique_ptr<std::atomic<float>[]> buf_;
    std::atomic<uint64_t> w_{ 0 };      // written by the producer only
    std::atomic<uint64_t> r_{ 0 };      // advanced by the consumer (reads) and the producer (drops)
    std::atomic<uint64_t> dropped_{ 0 };
};

// ---------------------------------------------------------------------------
// Lane -- one strip of the Live window.
// ---------------------------------------------------------------------------
class Lane {
public:
    Lane(const LiveInput& in, const std::string& kind);
    Lane(const Lane&) = delete;
    Lane& operator=(const Lane&) = delete;

    const std::string key;     // the source's key: "in:<id>", "out:<id>", "app:<exe>", "wav:<path>[@...]"
    const uint32_t    chan;    // 0 = the sum of the source's channels, 1..kLiveMaxChan = that channel alone
    const std::string kind;    // in | out | app | wav

    // The trim, changed IN PLACE when a replacement keeps the lane: the page re-sends the whole set on every trim
    // edit, and re-creating the lane would cost its FIFO -- 40-60 ms of zeros -- at each step of a slider.
    void   setGainDb(double db);
    double gainDb() const { return gainDb_.load(std::memory_order_relaxed); }

    // ---- the producer half: the source's thread ONLY --------------------------------------------------------
    // One packet: `frames` interleaved frames of `channels` channels at `rate` Hz (>= kAsrRate: a source asks
    // Windows for 16 kHz rather than hand the Decimator a lower rate -- TrDecimator.h). Returns false when the
    // strip's channel does not exist in this packet (the lane then carries zeros; the caller says so once).
    bool produce(const float* interleaved, uint32_t frames, uint32_t channels, uint32_t rate);
    // `frames` of silence at `rate` (AUDCLNT_BUFFERFLAGS_SILENT): through the Decimator like any audio, so its
    // history stays continuous.
    void produceSilence(uint32_t frames, uint32_t rate);

    LaneFifo fifo;

    // ---- the consumer half: the mixer thread ONLY -----------------------------------------------------------
    // n (1..kFeedMaxFrames) frames into out by the fill rule (see the constants above).
    void pull(float* out, int n);
    // The fill the mixer keeps for this lane, in 16 kHz frames: the kind's base, or more for big packets.
    int  target() const;

    // ---- observed from any thread ---------------------------------------------------------------------------
    struct Stats {
        uint64_t fedFrames = 0, dropped = 0, underruns = 0, slipsUp = 0, slipsDown = 0, resyncs = 0;
        double   fillMs = 0, targetMs = 0, periodMs = 0, decimatorMs = 0;
        bool     primed = false;
    };
    Stats  stats() const;
    // The front-end delay the contract's inputDelayMs promises: the source's packet period + the FIFO fill + the
    // Decimator's group delay. 0 until the lane has carried audio.
    double delayMs() const;

private:
    std::atomic<double> gainDb_{ 0.0 };
    std::atomic<float>  gain_{ 1.0f };

    // producer state
    Decimator          dec_;
    uint32_t           decRate_ = 0;
    std::vector<float> mono_, out16_;
    std::atomic<double> periodMs_{ 0.0 };       // EMA of the packet duration
    std::atomic<double> decimatorMs_{ 0.0 };
    std::atomic<int>    maxPacket16k_{ 0 };     // the largest packet seen, in 16 kHz frames

    // consumer state (mixer only, published through the atomics below)
    bool   primed_ = false;
    double ema_ = 0.0;
    int    sinceSlip_ = 0;
    int    highRun_ = 0;
    std::vector<float> tmp_;
    std::atomic<uint64_t> fedFrames_{ 0 }, underruns_{ 0 }, slipsUp_{ 0 }, slipsDown_{ 0 }, resyncs_{ 0 };
    std::atomic<double>   fillMs_{ 0.0 };
    std::atomic<bool>     primedPub_{ false };
};

// ---------------------------------------------------------------------------
// DeviceFeed -- the lanes bound to one source. The source's thread pushes packets; start() swaps the lane list
// (a strip added on another channel of a running device, one removed) without stopping the source.
// ---------------------------------------------------------------------------
class DeviceFeed {
public:
    explicit DeviceFeed(std::function<void(const std::string&)> log);
    void setLanes(std::vector<std::shared_ptr<Lane>> lanes);
    std::vector<std::shared_ptr<Lane>> lanes() const;
    // the source's thread ONLY
    void push(const float* interleaved, uint32_t frames, uint32_t channels, uint32_t rate);
    void pushSilence(uint32_t frames, uint32_t channels, uint32_t rate);

private:
    std::function<void(const std::string&)> log_;
    mutable std::mutex mu_;                      // held for a pointer copy, never while audio is processed
    std::vector<std::shared_ptr<Lane>> lanes_;
    std::vector<const Lane*> warned_;            // lanes already told their channel is missing (source thread)
};

// ---------------------------------------------------------------------------
// EventGate -- the ONLY way the hub reaches ILiveEvents (always created by make_shared). Two promises of
// TrCaptureApi.h are kept here:
//   * "after stop() returns no ILiveEvents call runs or will run": close() returns once no call is in flight, and
//     no call starts after it -- even from a thread the hub abandoned in a hung driver (that thread holds the gate,
//     closed, through a shared_ptr);
//   * "no call into ILiveEvents while holding a lock the caller of start/stop/requestSources might wait on": the
//     thread running start() or stop() holds the hub's lock, so it opens a Defer scope first -- its calls are
//     queued (by value) and delivered when the scope ends, which is after the lock is released.
// An exception thrown by an implementation is swallowed: it must not take a capture thread (and the engine) down.
// ---------------------------------------------------------------------------
class EventGate : public std::enable_shared_from_this<EventGate> {
public:
    explicit EventGate(ILiveEvents* events) : events_(events) {}

    void log(const std::string& line);
    void inputState(const std::string& key, const std::string& state, const std::string& why);
    void levels(const std::vector<std::pair<std::string, float>>& peaks);
    void sources(const std::vector<SourceInfo>& list);

    void close();
    bool closed() const { return closed_.load(); }

    // Declared BEFORE the hub's lock_guard: it ends after the lock is released, and delivers then.
    class Defer {
    public:
        Defer();
        ~Defer();
        Defer(const Defer&) = delete;
        Defer& operator=(const Defer&) = delete;
    private:
        std::vector<std::function<void()>>  q_;
        std::vector<std::function<void()>>* prev_;
    };

private:
    void route(std::function<void(ILiveEvents&)> f);
    void deliver(const std::function<void(ILiveEvents&)>& f);

    ILiveEvents*      events_;
    std::atomic<int>  inflight_{ 0 };
    std::atomic<bool> closed_{ false };
};

// ---------------------------------------------------------------------------
// StateReporter -- one source's INPUT state (one key: every strip on that source shares it). Sends on a CHANGE of
// state only (TrCaptureApi.h: "Never repeated for an unchanged state"); a new reason for the same state is logged,
// not sent.
// ---------------------------------------------------------------------------
class StateReporter {
public:
    StateReporter(std::string key, std::shared_ptr<EventGate> gate) : key_(std::move(key)), gate_(std::move(gate)) {}
    void set(const std::string& state, const std::string& why);
    std::string state() const;

private:
    const std::string key_;
    std::shared_ptr<EventGate> gate_;
    mutable std::mutex mu_;
    std::string state_;
};

// ---------------------------------------------------------------------------
// RetryPolicy -- how a failing source comes back (fix round RVA-8). Pure: the caller gives the clock (ms), so the
// WASAPI device thread runs it on GetTickCount64 and the wav: test source on its own clock (@gap, @flap), where the
// smoke proves it on the virtual clock.
//   * THE LADDER 0.2 / 1 / 2 / 5 s (then 5 s) advances on every failure and is reset ONLY by a stream that ran
//     kHealthyMs without failing: a device that opens and fails at once (a flapping driver) no longer cycles at 0.2 s.
//   * "open" is reported at once on a first open and on a reopen after a failure that was not a flap; once an open of
//     the episode failed within kHealthyMs (a FLAP), a reopen is reported only after kHealthyMs of health -- the failure
//     state stands meanwhile, instead of an INPUT open + error per cycle through the never-dropped queue.
//   * a failure is logged when its kind changes, a repeat once a minute ("still ..."); a held reopen is not logged
//     (the "steady again" line ends the episode).
// ---------------------------------------------------------------------------
class RetryPolicy {
public:
    static constexpr uint64_t kHealthyMs  = 5000;
    static constexpr uint64_t kStillLogMs = 60000;
    static uint32_t ladderMs(size_t step);            // 200 / 1000 / 2000 / 5000, 5000 thereafter

    // A stream opened at nowMs. true = report "open" (and log the open line) now; false = held (a flapping episode).
    bool opened(uint64_t nowMs);
    // While the stream runs (every wake of its loop): true ONCE, when a failing episode ENDS (healthy for kHealthyMs:
    // the ladder starts again at its first step). `reportOpen` = the open was held and is due now.
    bool healthy(uint64_t nowMs, bool& reportOpen);
    struct Failure {
        uint32_t waitMs = 0;      // before the next try
        bool     log = false;     // a new kind of failure: log it
        bool     still = false;   // the same one for a minute: log "still ..." instead
        unsigned tries = 0;       // failures in this episode
    };
    // The stream, or its opening, failed at nowMs. kind = what identifies a repeat (state|why|detail); retryMs != 0 =
    // the source's own wait (an application looked for by name, a privacy refusal) instead of the ladder's.
    Failure failed(uint64_t nowMs, const std::string& kind, uint32_t retryMs);
    bool inEpisode() const { return episode_; }

private:
    size_t      step_ = 0;            // the ladder's next step
    bool        episode_ = false;     // a failure, and no stream healthy since
    bool        flapping_ = false;    // ... and an open of this episode failed within kHealthyMs
    bool        held_ = false;        // the current stream's "open" was not reported
    bool        running_ = false;     // a stream is open
    uint64_t    openedAt_ = 0;
    std::string lastKind_;
    uint64_t    lastLogAt_ = 0;
    unsigned    tries_ = 0;
};

// ---------------------------------------------------------------------------
// BoundedThread -- a thread joined with a TIME LIMIT (std::thread has none). stop() must return within its bound
// even when a driver call never comes back: join(ms) then gives up and detaches; the body owns everything it touches
// through shared_ptr, so an abandoned thread finishes (or not) without reaching freed memory.
// ---------------------------------------------------------------------------
class BoundedThread {
public:
    BoundedThread() = default;
    BoundedThread(const BoundedThread&) = delete;
    BoundedThread& operator=(const BoundedThread&) = delete;
    ~BoundedThread();                          // detaches a thread still running (never std::terminate)

    void start(std::function<void()> body);
    bool started() const { return th_.joinable(); }
    // true = joined; false = still running after ms: detached (abandoned). Called from the thread itself it only
    // detaches (a thread cannot join itself) and returns true.
    bool join(DWORD ms);

private:
    struct Done;                               // the "finished" event, owned by the thread AND by this object
    std::thread           th_;
    std::shared_ptr<Done> done_;
};

// ---------------------------------------------------------------------------
// Source -- what the hub drives. One per distinct KEY: two strips on two channels of one interface are two lanes
// of ONE source (one open, one thread, one clock).
// ---------------------------------------------------------------------------
class Source {
public:
    virtual ~Source() = default;
    // Start working: a thread for a device or a real-clock WAV file; nothing for a source the mixer pumps.
    virtual void begin() = 0;
    // Wait until the first open attempt has finished (opened, or its state reported); true = it has.
    virtual bool waitFirstOpen(DWORD ms) = 0;
    virtual void requestStop() = 0;
    // Join the thread within ms; false = it is stuck (in a driver): abandoned, it can no longer reach the hub.
    virtual bool join(DWORD ms) = 0;
    // A device came, went or changed (IMMNotificationClient): a source waiting to reopen retries now.
    virtual void kick() {}
    // Virtual clock only: produce everything due up to hubSec seconds of the hub's clock (on the mixer thread).
    virtual void pump(double hubSec) { (void)hubSec; }
    virtual bool pumped() const { return false; }

    DeviceFeed& feed() { return *feed_; }
    const std::string& key() const { return key_; }

protected:
    Source(std::string key, std::shared_ptr<DeviceFeed> feed) : key_(std::move(key)), feed_(std::move(feed)) {}
    const std::string key_;
    std::shared_ptr<DeviceFeed> feed_;
};

// ---------------------------------------------------------------------------
// Sample formats (WAVEFORMATEX / WAVEFORMATEXTENSIBLE), decoded to interleaved float.
// ---------------------------------------------------------------------------
enum class SampleKind { Unknown, F32, F64, I16, I24, I32, U8 };

struct SampleFormat {
    uint32_t   rate = 0;
    uint32_t   channels = 0;
    uint32_t   blockAlign = 0;
    uint32_t   containerBytes = 0;
    SampleKind kind = SampleKind::Unknown;
};

// cb = the bytes available at wfx (a WAV file's fmt chunk may be short; WASAPI's are whole). false + why on a format
// this capture cannot decode (compressed, 0 channels...).
bool parseFormat(const WAVEFORMATEX* wfx, size_t cb, SampleFormat& out, std::string& why);
// frames x channels samples from data (blockAlign bytes per frame) into out (frames x channels floats).
void toFloat(const unsigned char* data, uint32_t frames, const SampleFormat& f, float* out);
// "48000 Hz, 2 ch, float32"
std::string describe(const SampleFormat& f);
// A WAVEFORMATEXTENSIBLE for 32-bit float at rate x channels (what the capture asks Windows for when it chooses).
WAVEFORMATEXTENSIBLE floatFormat(uint32_t rate, uint32_t channels);
// A WAVEFORMATEX for 16-bit PCM (the Application Loopback sample's own format, the fallback of an app capture).
WAVEFORMATEX pcm16Format(uint32_t rate, uint32_t channels);

// Small shared helpers.
std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);
std::string hr8(long hr);
std::string fmtMs(double ms);              // "12.3"

} // namespace cap
} // namespace pbt
