// =============================================================================
// SuVtitle - engine/TrAudio.h   (the live path's VAD: 16 kHz in, utterances out)
//
// SuVtitle 1.1.0 (2026-10-25): ported from AudioPB's translator/TrAudio.h at commit 50f79d6 (AudioPB Translator 1.1.0),
// for SuVtitleEngine.exe. In AudioPB this class read its audio from AudioPB's own routing; SuVtitle CAPTURES the audio
// itself (the capture hub, engine/TrCaptureApi.h). So the FRONT END is the one part replaced: the hub's MIXER thread
// hands this class 16 kHz blocks through ILiveSink (bindInputs / feed16k), and that thread is "the audio thread" every
// comment below talks about -- it owns the VAD's state while the live path runs, exactly as AudioPB's own audio thread
// did. Everything after the front end is 50f79d6's, verbatim: the VAD, the live line, the commit hand-off, the per-input
// energy that names the speaker, the bounded queue, the selftest path. The decimator moved to TrDecimator.h (the hub
// runs one per input; the selftest path keeps its own here).
//
// WHAT COMES OUT: 16 kHz MONO segments of speech, each carrying the per-input
// energy that produced it. Two facts travel with every segment and they are the
// whole point of doing the VAD here rather than inside the ASR:
//   * the segment is cut on ENERGY, so a pause in the conversation is a line
//     break in the subtitle -- the unit of translation is an utterance;
//   * the dominant input (highest RMS over the segment, measured PRE-MIX,
//     per input k) names the SPEAKER. Zero ML, exact by construction: whoever is
//     on that input (a strip of the Live window) is who talked. Two voices on ONE
//     input is TrSpeaker's job.
// =============================================================================
#pragma once

#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "TrCaptureApi.h"   // ILiveSink, kMaxLiveInputs
#include "TrDecimator.h"    // Decimator (the selftest path), kAsrRate

namespace pbt {

// ---------------------------------------------------------------------------
// VAD constants -- the spec's numbers, named so they can be argued with.
// ---------------------------------------------------------------------------
constexpr double kVadOpenDbfs  = -38.0;   // a hop above this OPENS a segment (spec section 3)
// Hysteresis: closing at the SAME level chatters on every syllable gap, cutting one
// utterance into confetti and multiplying the ASR calls. 6 dB below open is the
// classic gate ratio and costs nothing but a named constant.
constexpr double kVadCloseDbfs = -44.0;
constexpr int    kVadHopMs     = 30;      // RMS window; also the attribution's time grain
constexpr int    kHangoverMs   = 300;     // silence needed to CLOSE a segment (400 -> 300 in
                                          // the fluidity pass, user feedback: tighter phrase
                                          // chaining; still 10 VAD hops of hysteresis)
constexpr int    kMinSegMs     = 600;     // shorter than this = a door slam, not a sentence
constexpr int    kMaxSegMs     = 8000;    // force-cut: a monologue must not delay its own subtitle
                                          // -- and since W3 it bounds the UNCOMMITTED part
                                          // only, because the committed part is already on
                                          // screen and delays nobody.
constexpr int    kPreRollMs    = 200;     // kept BEFORE the open, so the first word survives

// ---------------------------------------------------------------------------
// PROGRESSIVE DISPLAY (the live line).
//
// The numbers above are why this exists. A subtitle cannot appear before its
// segment CLOSES, and a segment closes 400 ms after the speaker stops -- or, in
// continuous speech, not until the 8 s force-cut. So the honest floor on a
// finished line is "when the sentence ended", and on a monologue it is eight
// seconds. Nothing in the ASR can fix that; it is the segmentation.
//
// A partial is therefore a SECOND, cheaper product of the same open segment: the
// audio so far, snapshotted every kPartialSec, transcribed as a draft. It never
// replaces the final -- it holds the screen until the final arrives.
// /!\ THE MASTER COPY of this default; TrConfig::partialSec (TrEngine.h) must match it.
// It said 1.2 until W2 while TrConfig said 0.6, so a helper whose config file had no
// PARTIALSEC line ran at half the cadence of one that did -- two answers to one question,
// which is exactly the shape of the v1 bug the config migration exists to undo.
constexpr double kPartialSecDefault = 0.6;   // cadence, seconds (config: PARTIALSEC)
constexpr double kPartialSecMin     = 0.5;
constexpr double kPartialSecMax     = 5.0;
// A snapshot reaches back at most this far. kMaxSegMs (8 s) already bounds an open
// segment, so this is a belt on top of a brace: the cost of one partial inference
// must be bounded by a constant in this file, not by a constant in another one.
constexpr int    kPartialMaxMs      = 12000;

// One snapshot of an OPEN segment. Not a Segment: it has no attribution, no cut
// time and no permanence -- three things a draft has no business carrying.
//
// W3 added the two fields that turn a draft into something that can be COMMITTED: where
// this window sits inside its segment, and when (wall clock) the segment's first sample
// was heard. Between them the pipeline can turn a word's timestamp -- which whisper gives
// relative to the WAV it was sent -- into a segment-relative sample offset and into an
// instant on the clock, which is what a commit offset and an honest waitMs are made of.
struct PartialSnap {
    uint64_t           segId = 0;        // the open utterance; its final carries the same id
    std::vector<float> mono16k;          // 16 kHz mono, LAST COMMIT (or the last 12 s) -> now
    uint64_t           tickMs = 0;       // GetTickCount64 at the snapshot
    double             peakDbfs = -120.0;   // loudest hop so far -- feeds the hallucination filter
    size_t             startOffset = 0;  // this window's first sample, counted from the segment's
    uint64_t           segStartTickMs = 0;  // wall clock of the segment's FIRST sample (pre-roll
                                            // included) -- sample 0 of offset 0, not of this window
    // The dominant input AS OF THIS SNAPSHOT, by the same arithmetic the cut uses.
    // A committed sentence is a FINAL and needs the speaker KEY the app builds from
    // {input, voice}: without this a mid-utterance line and its closing line would claim
    // two different speakers for one breath.
    std::string        srcSlot;
    double             seconds() const { return (double)mono16k.size() / (double)kAsrRate; }
};

// One cut utterance, ready for the ASR. Since W3 it is the TAIL of that utterance: the
// part that was still uncommitted when the VAD closed (startOffset says how much came
// before it, and that much has already been sent as LINEs).
struct Segment {
    std::vector<float> mono16k;                    // the audio, 16 kHz mono, [-1,1]
    uint64_t           segId = 0;                  // pairs this final with its PARTs
    double             srcEnergy[kMaxLiveInputs] = {};   // per-INPUT mean square over the segment
    std::string        srcName[kMaxLiveInputs];          // that input's key (TrPipe.h LIVE: in: / out: / app: ...)
    double             peakDbfs = -120.0;          // loudest hop -- feeds the hallucination filter
    uint64_t           cutTickMs = 0;              // GetTickCount64 at the cut (latency measurement)
    size_t             startOffset = 0;            // where this tail starts inside its segment.
                                                   // > 0 means sentences were already emitted;
                                                   // the pipeline checks it against what it
                                                   // ASKED to commit (see commitUpTo).
    uint64_t           segStartTickMs = 0;         // wall clock of the segment's first sample
    bool               closes = true;              // a queued segment always ends its utterance;
                                                   // carried explicitly because the pipeline's
                                                   // wire field of the same name is not derived

    // The speaker attribution, v1: the loudest input wins. Returns "" when nothing
    // was bound (a segment can still exist: the mix is the sum of zero inputs
    // only if something else made noise, which cannot happen -- so "" means the
    // binding changed mid-segment).
    std::string dominantSource() const {
        int best = -1;
        double bestE = 0.0;
        for (uint32_t k = 0; k < kMaxLiveInputs; ++k)
            if (!srcName[k].empty() && srcEnergy[k] > bestE) { bestE = srcEnergy[k]; best = (int)k; }
        return best < 0 ? std::string() : srcName[(size_t)best];
    }
};

// (The windowed-sinc Decimator that stood here -- any capture rate -> 16 kHz -- is TrDecimator.h's now: the capture
// hub runs one per input, and the selftest path below still runs one of its own.)

// ---------------------------------------------------------------------------
// The VAD and its hand-offs. While the live path runs, the capture hub's MIXER thread
// feeds it (ILiveSink: bindInputs, feed16k) and owns every "audio thread only" member
// below; segments leave through pop() on the pipeline thread.
// ---------------------------------------------------------------------------
class TrAudio : public ILiveSink {
public:
    TrAudio() = default;
    ~TrAudio() override = default;   // nothing runs: the hub is stopped before this dies (runHost)

    TrAudio(const TrAudio&) = delete;
    TrAudio& operator=(const TrAudio&) = delete;

    // The live path starts / stops. PIPELINE THREAD, and only while no mixer thread exists -- before
    // TrCaptureHub::start of a new run, after TrCaptureHub::stop -- so the hand-over of the audio thread's
    // state is the hub's thread start / join. beginLive is AudioPB's start() + the prologue of its audio
    // thread (the hop, the pre-roll, the accumulator history), endLive its stop(). Neither empties the queue
    // or re-numbers the utterances: the pipeline clears what it wants cleared (clear()), and an utterance id
    // is never re-used within one engine (the pipeline's "already on screen" test is an id comparison).
    void beginLive();
    void endLive();
    // LIVE 0 (SuVtitle 1.1.0, fix round RVA-1): the capture has STOPPED, and the end of the input is a close -- the
    // end of a selftest file's rule (selftestEnd): the open utterance becomes a final when it is long enough to be a
    // sentence or something of it was committed; a shorter one is dropped. PIPELINE THREAD, only after
    // TrCaptureHub::stop (the mixer joined: the audio thread's state is the caller's). A Clear no block came to apply
    // is honoured first: the utterance the user asked to forget is not cut into a final.
    void endOfInput();

    bool running() const { return running_.load(std::memory_order_relaxed); }

    // ---- ILiveSink: the capture hub's MIXER thread only (TrCaptureApi.h) ----
    // The input keys per lane ("" = an empty lane): the speaker attribution's names.
    void bindInputs(const std::string (&keys)[kMaxLiveInputs]) override;
    // One block of the mix at 16 kHz, and each lane's own contribution to it (the per-input energy).
    void feed16k(const float* mix, int n, const float* const (&perInput)[kMaxLiveInputs]) override;

    // Live facts for STATE: is anything bound, and how loud is it right now.
    int    boundSources() const { return boundCount_.load(std::memory_order_relaxed); }
    double lastHopDbfs()  const { return lastDb_.load(std::memory_order_relaxed); }

    // Blocking pop with a deadline; false = nothing within `ms` (or we are stopping).
    bool pop(Segment& out, uint32_t ms);
    // Drop everything queued AND abandon the segment being cut (the CTL clear).
    void clear();

    // How many segments the queue had to drop because the pipeline fell behind.
    uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

    // Is a FINISHED utterance waiting? The live line's scheduler asks this at the one
    // moment it can still change its mind -- between a draft's ASR and its translation
    // -- so that a final never waits behind the cheap half of a draft it preempts.
    bool finalWaiting() const;

    // ---- the LIVE LINE tap ---------------------------------------------------
    // Cadence in seconds; 0 (or anything <= 0) turns partials off entirely. Set it
    // BEFORE beginLive()/selftestBegin -- it is read on the audio thread, so it lives in
    // an atomic and a late change simply takes effect at the next snapshot.
    void setPartialSec(double sec);

    // NON-BLOCKING, and the only way out of the partial slot. false = nothing
    // pending. `out`'s buffer is SWAPPED into the slot rather than copied, so the
    // audio thread keeps reusing one allocation instead of making one per snapshot.
    bool popPartial(PartialSnap& out);

    // "The final for this utterance has been dealt with" -- drop any partial that is
    // now moot. Called by the pipeline the moment it pops a final; the audio thread
    // already does the same thing at the cut, and neither can be the only one to.
    void dropPartialsUpTo(uint64_t segId);

    // ---- the COMMIT hand-off (W3) --------------------------------------------
    // "Everything before `sampleOffset` of segment `segId` is finalised; stop sending it
    // to me." The offset is SEGMENT-RELATIVE (the same coordinate PartialSnap::startOffset
    // and Segment::startOffset are in), and the segment shrinks from the front: the open
    // segment then holds only the uncommitted audio, which is what makes the 8 s force-cut
    // a bound on the WAIT rather than on the utterance.
    //
    // Pipeline -> audio thread, applied at the audio thread's next hop, because open_/cur_
    // are that thread's state and a vector two threads resize is a crash, not a feature.
    // IGNORED when `segId` is no longer the open segment (the VAD closed it meanwhile) and
    // when the offset does not MOVE FORWARD. /!\ The ignored case is not silent on the other
    // side: the tail this misses was already queued with the OLD offset, so the pipeline
    // compares Segment::startOffset with what it asked for and trims the overlap itself.
    void commitUpTo(uint64_t segId, size_t sampleOffset);

    uint64_t partialsMade()    const { return pMade_.load(std::memory_order_relaxed); }
    // Snapshots overwritten before anyone read them. NOT a fault: it is the design
    // (latest-wins), and this counter is how the cadence claim gets checked.
    uint64_t partialsSkipped() const { return pSkipped_.load(std::memory_order_relaxed); }

    // ---- SELFTEST path: the same VAD + queue, fed from a file instead of the
    // capture. No hub, no thread, no clock -- everything else identical, which is
    // exactly what makes it a proof of the shipping code rather than of a copy.
    void selftestBegin(uint32_t inRate, const std::string& fakeSourceName);
    void selftestFeed(const float* mono, int n);
    void selftestEnd();

private:
    // (SuVtitle 1.1.0: AudioPB's front end -- threadMain, the binding of its routing's inputs, the gains -- is the
    // capture hub's job now, which hands this class its result through bindInputs / feed16k.)

    // The VAD proper, fed 16 kHz mono. Runs on the audio thread (and, in selftest,
    // on the caller's). Pushes finished segments into the queue.
    void vadPush(const float* mono16k, int n);
    void vadOpen();
    void vadCut(bool forced);
    void pushSegment(Segment&& s);
    void maybePublishPartial();      // audio thread: one snapshot, if one is due
    void dropPartial();              // audio thread + clear(): forget the pending snapshot
    void applyCommit();              // audio thread: honour a pending commitUpTo request
    // Per-input mean square SINCE THE OPEN, from the accumulator history. ONE expression,
    // used by the cut (which carries the whole array) and by a snapshot (which only needs
    // the winner's name) -- so a live line and its final can never disagree about the input.
    void energySinceOpen(double (&out)[kMaxLiveInputs]) const;

    std::atomic<bool> running_{ false };
    std::atomic<bool> stop_{ false };
    std::atomic<int>  boundCount_{ 0 };
    std::atomic<double> lastDb_{ -120.0 };
    std::atomic<uint64_t> dropped_{ 0 };
    std::atomic<bool> clearAsked_{ false };   // CTL clear -> the audio thread drops its cut

    // ---- the inputs (audio thread only) ----
    std::string      boundName_[kMaxLiveInputs];   // each lane's input key, for attribution

    // ---- 16 kHz front end ----
    Decimator          dec_;         // the SELFTEST path's (a bench file at its own rate); live audio
                                     // arrives at 16 kHz, decimated per input by the hub
    std::vector<float> mono16_;      // scratch: the guarded live block / the decimator's output

    // ---- per-input energy accumulators (audio thread only) ----
    // Monotonic sum-of-squares per INPUT, snapshotted once per hop into a small
    // history. A segment's per-input energy is then the DIFFERENCE between two
    // snapshots -- which is what makes the attribution exact and pre-mix without
    // buffering four full copies of the audio.
    static constexpr int kAccHistory = 128;          // ~3.8 s of hops: covers the pre-roll with room
    double accSum_[kMaxLiveInputs] = {};
    double accN_ = 0.0;
    struct AccSnap { double sum[kMaxLiveInputs]; double n; };
    std::vector<AccSnap> accHist_;                   // ring of kAccHistory snapshots
    int                  accHead_ = 0;               // next write index
    int                  accCount_ = 0;              // snapshots taken (caps the look-back)
    void snapshotAcc();
    const AccSnap& accAgo(int hops) const;           // snapshot `hops` hops in the past (clamped)

    // ---- VAD state (audio thread only) ----
    int                hopSamples_ = kAsrRate * kVadHopMs / 1000;
    std::vector<float> hop_;         // partial hop being filled
    std::vector<float> preRoll_;     // rolling pre-roll ring (kPreRollMs at 16 kHz)
    size_t             preRollAt_ = 0;
    bool               preRollFull_ = false;
    bool               open_ = false;
    int                hangoverHops_ = 0;
    Segment            cur_;                          // the segment being cut -- since W3 only
                                                      // its UNCOMMITTED part (see commitOff_)
    AccSnap            curStart_{};                   // accumulator at its (pre-rolled) start
    std::string        curNames_[kMaxLiveInputs];     // input keys AT THE OPEN (a rebind mid-
                                                      // segment must not relabel the speaker)
    // How much of this segment has been finalised already, in samples from its start.
    // cur_.mono16k holds [commitOff_, now); the committed prefix is ERASED rather than
    // kept, because a monologue where every sentence commits would otherwise never
    // force-cut and the buffer would grow for as long as the person talks.
    size_t             commitOff_ = 0;
    uint64_t           curOpenTickMs_ = 0;            // wall clock of cur_'s FIRST sample

    // ---- the bounded hand-off to the pipeline thread ----
    static constexpr size_t kQueueCap = 8;   // ~64 s of speech in flight: past that we are
                                             // not "behind", we are broken -- drop the OLDEST
                                             // (a stale subtitle is worse than a missing one)
    mutable std::mutex      qmx_;
    std::condition_variable qcv_;
    std::deque<Segment>     queue_;

    // ---- the LIVE LINE hand-off ------------------------------------------------
    // /!\ A SEPARATE MUTEX, AND ONE SLOT. Both halves of that matter:
    //   * separate, because the partial path must never be able to make the audio
    //     thread wait on a lock the FINAL path holds. The two hand-offs share no
    //     state, so they share no lock, and a partial can never delay a cut;
    //   * one slot, latest-wins, because a partial that could not be picked up is
    //     WORTHLESS -- a newer one has already superseded it. A queue here would
    //     turn a busy pipeline into a backlog of stale drafts, each costing an ASR
    //     call to display something the speaker has already moved past.
    // There is therefore no such thing as "partials falling behind": they are
    // skipped, counted (pSkipped_) and forgotten.
    mutable std::mutex    pmx_;
    PartialSnap           pend_;
    bool                  pendHas_ = false;
    // ---- the COMMIT request slot (pipeline -> audio thread) --------------------
    // Its OWN mutex, for the reason pmx_ has its own: the audio thread must never be able
    // to wait on a lock the final queue holds. Two words wide and held for two stores, so
    // the "lock on the audio thread" objection costs nothing measurable -- and packing the
    // pair into one atomic would mean inventing a bit layout for a segment id, which is a
    // trap waiting for the session that runs long enough to overflow it.
    mutable std::mutex    cmx_;
    uint64_t              creqSeg_ = 0;      // 0 = nothing asked for
    size_t                creqOff_ = 0;
    std::atomic<uint32_t> partialSamples_{ 0 };   // 0 = the live line is off
    std::atomic<uint64_t> pMade_{ 0 };
    std::atomic<uint64_t> pSkipped_{ 0 };
    // Audio-thread only: the id of the open segment, and the buffer length at which
    // the next snapshot comes due.
    uint64_t              nextSegId_ = 0;
    size_t                nextPartialAt_ = 0;
    // Audio-thread only. The snapshot is COPIED into here with the lock OPEN and only
    // then SWAPPED into the slot, so the audio thread's critical section is O(1)
    // pointer moves rather than a 12 s memcpy. Three buffers end up cycling between
    // this, pend_ and the caller's PartialSnap -- which is why a steady stream of
    // partials allocates nothing at all after the first few.
    std::vector<float>    pscratch_;
};

} // namespace pbt
