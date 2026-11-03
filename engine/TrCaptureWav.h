// =============================================================================
// SuVtitle - engine/TrCaptureWav.h   (the "wav:" TEST source: a WAV file played into the hub as if it were a device)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b (SUVTITLE_LIVE_PLAN section 4.8). A TEST ENGINE ONLY source
// (HubOptions::testSources): it goes through the REAL hub -- the lane, its Decimator, its FIFO, the slip rule, the
// mix -- which makes it a proof of the shipped code that selftestFeed (which bypasses the thread and the clock) is
// not. It imitates an INPUT: one packet per 10 ms of its own clock, silence after the end of the file (a quiet
// microphone), INPUT stalled when it sends nothing for 2 s.
//
//   wav:<path>[@rate=<Hz>][@drift=<+-ppm>][@gap=<t>:<dur>][@flap=<t>:<dur>][@stall=<t>:<dur>][@loop]
//     @rate=   the file is resampled (linearly: a test signal, not a master) to this "device" rate -- the
//              Decimator then sees that rate. Below 16 kHz: refused (INPUT error), as the hub never hands the
//              Decimator a rate below its output (TrDecimator.h).
//     @drift=  the source's clock runs this many ppm fast (+) or slow (-) against the hub's: it delivers more (or
//              fewer) frames per second than the mixer takes, which is what the slip rule is for.
//     @gap=    from t s for dur s of the source's clock the "device is unplugged": no packet, INPUT absent; it is
//              REOPENED on RetryPolicy's ladder, as a device is (fix round RVA-8: tries 0.2, 1, 2, 5 s after the
//              loss -- a try inside the gap fails, the first one after it opens), its audio until then lost. Repeatable.
//     @flap=   from t s for dur s every open FAILS 50 ms after it succeeded (a driver that opens and errors on its first
//              reads): INPUT error, and the reopens follow the ladder without resetting it -- each delivers its 50 ms --
//              while INPUT stays "error" until a stream has run RetryPolicy::kHealthyMs. Repeatable.
//     @stall=  from t s for dur s the source sends NOTHING while staying open -- a driver that stopped delivering:
//              zeros reach the VAD at a steady cadence, and after 2 s INPUT says stalled. Repeatable.
//     @loop    restart the file at its end (long drift runs) instead of trailing silence.
//   The options are read from the END of the key, so a path that contains '@' still works.
//
// On the real clock it runs its own thread (QPC); on the virtual clock (HubOptions::virtualClock) the hub's mixer
// pumps it in lockstep, so a 10-minute drift run takes seconds and gives the same result every time.
// =============================================================================
#pragma once

#include "TrCaptureCore.h"

namespace pbt {
namespace cap {

struct WavOptions {
    std::string path;                                   // UTF-8
    uint32_t    rate = 0;                               // 0 = the file's own rate
    double      driftPpm = 0.0;
    std::vector<std::pair<double, double>> gaps;        // {t, dur} in seconds of the source's own clock
    std::vector<std::pair<double, double>> flaps;
    std::vector<std::pair<double, double>> stalls;
    bool        loop = false;
};

// false + why on a malformed option or an empty path.
bool parseWavKey(const std::string& key, WavOptions& o, std::string& why);

class WavSource : public Source, public std::enable_shared_from_this<WavSource> {
public:
    WavSource(const std::string& key, std::shared_ptr<EventGate> gate, bool virtualClock);
    ~WavSource() override;

    // The file is read by whoever produces it -- the source's own thread on the real clock, the mixer's first pump
    // on the virtual one -- so every INPUT state of this key comes from ONE thread, in order.
    void begin() override;
    bool waitFirstOpen(DWORD ms) override;
    void requestStop() override;
    bool join(DWORD ms) override;
    void pump(double hubSec) override;
    bool pumped() const override { return virtual_; }

private:
    bool open();                        // false = INPUT error said, nothing will ever be produced
    void threadMain();
    void produceUntil(double hubSec);
    void fillPacket();
    void fail(double t, bool gap);      // the "device" fails at t (s of its clock): RetryPolicy says when to try again

    std::shared_ptr<EventGate> gate_;
    StateReporter              reporter_;
    const bool                 virtual_;
    WavOptions                 opt_;
    bool                       ok_ = false;

    std::vector<float> pcm_;            // interleaved, at rate_
    uint32_t rate_ = 0, ch_ = 0;
    uint64_t frames_ = 0;
    uint32_t packet_ = 0;               // frames per 10 ms packet
    std::vector<float> pkt_;

    // production state (one thread at a time: the WAV thread, or the mixer on the virtual clock)
    std::mutex pumpMu_;
    uint64_t emitted_ = 0;              // device frames produced (or lost in a gap / stall)
    uint64_t content_ = 0;              // read position in pcm_
    double   lastPushDev_ = 0.0;        // device seconds at the end of the last packet pushed
    bool     stalled_ = false;
    // fix round RVA-8: the "device" closed after a failure (@gap / @flap) and reopened on RetryPolicy's ladder
    RetryPolicy retry_;
    bool     down_ = false;
    double   retryAt_ = 0.0;            // device seconds of the next try
    double   openedAt_ = 0.0;           // device seconds of the last (re)open

    bool          tried_ = false;       // open() has run (pump path)
    HANDLE        stopEv_ = nullptr;
    HANDLE        firstEv_ = nullptr;   // set once open() has run, whatever its outcome
    BoundedThread thread_;
};

} // namespace cap
} // namespace pbt
