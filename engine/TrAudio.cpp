// =============================================================================
// SuVtitle - engine/TrAudio.cpp
//
// SuVtitle 1.1.0 (2026-10-25): ported from AudioPB's translator/TrAudio.cpp at commit 50f79d6 (AudioPB Translator
// 1.1.0). The VAD, the live line, the commit hand-off and the selftest path are 50f79d6's, verbatim; the front end
// (its routing's inputs, the decimator, the clock that owed it frames) is the capture hub's now -- see TrAudio.h.
// =============================================================================
#include "TrAudio.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace pbt {

// (SuVtitle 1.1.0: the front end's kBlock / kTargetFill, its QPC clock and its MMCSS registration were here. The
// capture hub owns the device threads and the mixer's clock now; the Decimator moved to TrDecimator.cpp.)

// The pre-roll reaches back this many hops (the 200 ms plus the hop that triggered).
static constexpr int kPreRollHops = (kPreRollMs + kVadHopMs + kVadHopMs - 1) / kVadHopMs;

static inline double dbfs(double rms) {
    return rms > 1.0e-9 ? 20.0 * std::log10(rms) : -180.0;
}

// =============================================================================
// TrAudio -- lifecycle (SuVtitle 1.1.0: the capture hub feeds it; see TrAudio.h)
// =============================================================================
// AudioPB's start() and the prologue of its audio thread, minus what the hub took over (the routing's slot, the
// decimator's rate): the hop, the pre-roll ring, the accumulator history. The utterance numbering, the queue and the
// open segment are left alone -- the pipeline clears what it wants cleared (clear()), and the first block fed applies it.
void TrAudio::beginLive() {
    stop_.store(false);
    hopSamples_ = kAsrRate * kVadHopMs / 1000;
    hop_.clear();
    hop_.reserve((size_t)hopSamples_);
    preRoll_.assign((size_t)((kPreRollMs + kVadHopMs) * kAsrRate / 1000), 0.0f);
    preRollAt_ = 0; preRollFull_ = false;
    accHist_.assign((size_t)kAccHistory, AccSnap{});
    accHead_ = 0; accCount_ = 0;
    running_.store(true);
}

void TrAudio::endLive() {
    stop_.store(true);
    running_.store(false);
    qcv_.notify_all();
}

// The capture stopped (fix round RVA-1): what feed16k's Clear would have done, if one is pending -- else the end of the
// input is a close, by the selftest path's own rule (selftestEnd, verbatim: applyCommit first, the queue, the reset).
void TrAudio::endOfInput() {
    if (clearAsked_.exchange(false, std::memory_order_acq_rel)) {
        open_ = false;
        cur_ = Segment{};
        commitOff_ = 0;
        { std::lock_guard<std::mutex> lk(cmx_); creqSeg_ = 0; }
        hop_.clear();
        dropPartial();
        return;
    }
    selftestEnd();
}

// The binding's one job that stayed here: knowing WHICH INPUT each lane is -- the attribution's name, and the only
// moment that is unambiguous is the bind. (AudioPB re-validated its routing's sources here on every pass; the hub
// does the equivalent for the devices and calls this when the set changes.) A lane re-bound mid-utterance does not
// relabel it: vadOpen froze the names that utterance is attributed with.
void TrAudio::bindInputs(const std::string (&keys)[kMaxLiveInputs]) {
    int nb = 0;
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) {
        boundName_[k] = keys[k];
        if (!boundName_[k].empty()) ++nb;
    }
    boundCount_.store(nb, std::memory_order_relaxed);
}

// One block of the audio thread's old loop, from the point where the mix existed: the clear flag, each input's own
// energy, the guard, the VAD.
void TrAudio::feed16k(const float* mix, int n, const float* const (&perInput)[kMaxLiveInputs]) {
    if (!mix || n <= 0) return;

    // The pipeline thread asked for a Clear: drop whatever is half-cut. Done HERE
    // rather than from that thread, because cur_/open_ are this thread's state and
    // a bool written from two threads is a race, not a feature.
    if (clearAsked_.exchange(false, std::memory_order_acq_rel)) {
        open_ = false;
        cur_ = Segment{};
        commitOff_ = 0;
        // A commit still in flight names a segment the user has just asked us to
        // forget; applying it to whatever opens next would trim a stranger's audio.
        { std::lock_guard<std::mutex> lk(cmx_); creqSeg_ = 0; }
        hop_.clear();
        dropPartial();      // the live line goes with the log it was living under
    }

    // Each input's own contribution, after its trim: exactly what it puts into the mix,
    // which is what "who is loudest" has to mean. A muted or trimmed-out input
    // contributes zero and can never win the attribution.
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) {
        const float* in = perInput[k];
        if (!in) continue;
        double e = 0.0;
        for (int f = 0; f < n; ++f) {
            const double v = (double)in[f];
            e += v * v;
        }
        accSum_[k] += e;
    }
    accN_ += (double)n;

    // NaN/inf guard + a generous ceiling. The segment is peak-normalised before it
    // reaches the ASR, so this clamp only has to stop the maths going non-finite.
    // (The hub guards each input already, TrCaptureApi.h; the sum the VAD stands on is
    // guarded here all the same, as it always was.)
    mono16_.assign(mix, mix + n);
    for (int f = 0; f < n; ++f) {
        float v = mono16_[(size_t)f];
        if (!(v == v)) v = 0.0f;
        else if (v >  4.0f) v =  4.0f;
        else if (v < -4.0f) v = -4.0f;
        mono16_[(size_t)f] = v;
    }
    vadPush(mono16_.data(), n);
}

// =============================================================================
// Per-input energy accumulator history
// =============================================================================
void TrAudio::snapshotAcc() {
    AccSnap s{};
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) s.sum[k] = accSum_[k];
    s.n = accN_;
    if (accHist_.empty()) accHist_.assign((size_t)kAccHistory, AccSnap{});
    accHist_[(size_t)accHead_] = s;
    accHead_ = (accHead_ + 1) % kAccHistory;
    if (accCount_ < kAccHistory) ++accCount_;
}

const TrAudio::AccSnap& TrAudio::accAgo(int hops) const {
    static const AccSnap kZero{};
    if (accHist_.empty() || accCount_ == 0) return kZero;
    if (hops < 0) hops = 0;
    if (hops > accCount_ - 1) hops = accCount_ - 1;
    int idx = accHead_ - 1 - hops;
    while (idx < 0) idx += kAccHistory;
    return accHist_[(size_t)idx];
}

// =============================================================================
// VAD -- energy gate over 30 ms hops, with pre-roll and hangover.
// =============================================================================
void TrAudio::vadPush(const float* mono16k, int n) {
    const size_t maxSamples = (size_t)(kMaxSegMs * kAsrRate / 1000);
    const size_t minSamples = (size_t)(kMinSegMs * kAsrRate / 1000);
    const int    hangHops   = (kHangoverMs + kVadHopMs - 1) / kVadHopMs;

    for (int i = 0; i < n; ++i) {
        const float s = mono16k[i];
        hop_.push_back(s);
        if (!preRoll_.empty()) {
            preRoll_[preRollAt_] = s;
            preRollAt_ = (preRollAt_ + 1) % preRoll_.size();
            if (preRollAt_ == 0) preRollFull_ = true;
        }
        if ((int)hop_.size() < hopSamples_) continue;

        // ---- one hop is complete: measure, snapshot, decide -------------------
        double sum = 0.0;
        for (float v : hop_) sum += (double)v * (double)v;
        const double db = dbfs(std::sqrt(sum / (double)hop_.size()));
        lastDb_.store(db, std::memory_order_relaxed);
        snapshotAcc();

        // BEFORE the cut tests, and that ordering is the whole race: a commit the pipeline
        // asked for while this hop was being filled must be applied to the segment that
        // the SAME hop may be about to close, or the tail would carry a sentence that is
        // already on screen. (The pipeline still checks -- there is a window where the cut
        // came first -- but this is what makes that window rare rather than routine.)
        applyCommit();

        if (!open_) {
            if (db > kVadOpenDbfs) vadOpen();       // pre-roll (this hop included) -> cur_
        } else {
            cur_.mono16k.insert(cur_.mono16k.end(), hop_.begin(), hop_.end());
            if (db > cur_.peakDbfs) cur_.peakDbfs = db;
            if (db < kVadCloseDbfs) ++hangoverHops_; else hangoverHops_ = 0;
            if (hangoverHops_ >= hangHops) {
                // Close. A segment shorter than kMinSegMs is a door slam / a chair, not
                // a sentence: dropping it here saves an ASR round trip AND spares the user
                // whisper's favourite hallucination on 300 ms of noise.
                //
                // /!\ UNLESS SOMETHING WAS COMMITTED (W3). Then this breath-length tail is
                // not a door slam, it is the END OF AN UTTERANCE THE USER IS READING: the
                // live line under it must fall, and only a message can make it fall. So it
                // is queued, and the pipeline turns it into a LINE or into an END.
                if (cur_.mono16k.size() >= minSamples || commitOff_ > 0) vadCut(false);
                else { open_ = false; cur_ = Segment{}; commitOff_ = 0; }
            } else if (cur_.mono16k.size() >= maxSamples) {
                vadCut(true);       // force-cut: a monologue must not delay its own subtitle
            } else {
                // Still open, still talking. THIS is the only place a live line is born,
                // and it is deliberately after both cut tests: a snapshot taken on the hop
                // that closes the segment would be superseded by the final microseconds
                // later, having cost a whole ASR call to say nothing new.
                maybePublishPartial();
            }
        }
        hop_.clear();
    }
}

void TrAudio::vadOpen() {
    open_ = true;
    hangoverHops_ = 0;
    cur_ = Segment{};
    // The utterance's identity, minted HERE and nowhere else. A force-cut re-opens
    // immediately (see vadCut), so the continuation of a monologue is a NEW id -- which
    // is right: it becomes its own paragraph on screen, and its own live line.
    cur_.segId = ++nextSegId_;
    cur_.mono16k.reserve((size_t)(kMaxSegMs * kAsrRate / 1000) + (size_t)preRoll_.size());
    // The pre-roll ring, oldest first. Without it the gate eats the attack of the first
    // word, which is exactly the syllable the ASR needs to pick the language.
    if (!preRoll_.empty()) {
        if (preRollFull_) {
            cur_.mono16k.insert(cur_.mono16k.end(), preRoll_.begin() + (long)preRollAt_,
                                preRoll_.end());
            cur_.mono16k.insert(cur_.mono16k.end(), preRoll_.begin(),
                                preRoll_.begin() + (long)preRollAt_);
        } else {
            cur_.mono16k.insert(cur_.mono16k.end(), preRoll_.begin(),
                                preRoll_.begin() + (long)preRollAt_);
        }
    }
    curStart_ = accAgo(kPreRollHops);
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) curNames_[k] = boundName_[k];
    commitOff_ = 0;
    // WHEN SAMPLE ZERO OF THIS SEGMENT WAS HEARD. `now` is the end of the hop that opened
    // the gate, and everything already in cur_ (the pre-roll, plus that hop) happened
    // before it -- so the instant of the first sample is now minus what we are holding.
    // It is deliberately NOT "now": a word's timestamp is measured from sample 0, and a
    // waitMs built on the wrong origin would be wrong by the pre-roll, every time.
    // (What it cannot model is the capture's own front-end delay -- the device period,
    // the hub's FIFO, the decimators; the servant adds the hub's measure of it for the
    // dominant input, TrCaptureHub::inputDelayMs, when it stamps waitMs.)
    {
        const uint64_t heldMs = (uint64_t)cur_.mono16k.size() * 1000ull / (uint64_t)kAsrRate;
        const uint64_t now = ::GetTickCount64();
        curOpenTickMs_ = now > heldMs ? now - heldMs : 0ull;
    }

    // Arm the live line. The threshold is an ABSOLUTE buffer length, so the first
    // snapshot goes out once the segment holds PARTIALSEC of audio (pre-roll included,
    // as the spec words it) and every PARTIALSEC of growth after that.
    const size_t cadence = (size_t)partialSamples_.load(std::memory_order_relaxed);
    nextPartialAt_ = cadence ? cadence : (size_t)-1;
}

void TrAudio::energySinceOpen(double (&out)[kMaxLiveInputs]) const {
    const AccSnap& end = accAgo(0);
    const double dn = end.n - curStart_.n;
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) {
        out[k] = (dn > 0.0) ? (end.sum[k] - curStart_.sum[k]) / dn : 0.0;
        if (out[k] < 0.0) out[k] = 0.0;      // a wrapped snapshot, not signal
    }
}

// The pipeline's commit request, honoured on THIS thread. Everything it can refuse it
// refuses silently -- the caller has a second line of defence (Segment::startOffset) and
// a request that lost its race is not an error, it is a race that was lost.
void TrAudio::applyCommit() {
    uint64_t seg = 0;
    size_t   off = 0;
    {
        std::lock_guard<std::mutex> lk(cmx_);
        seg = creqSeg_;
        off = creqOff_;
        creqSeg_ = 0;
    }
    if (!seg || !open_ || seg != cur_.segId) return;
    if (off <= commitOff_) return;                       // offsets only ever move forward
    size_t drop = off - commitOff_;
    if (drop > cur_.mono16k.size()) drop = cur_.mono16k.size();   // cannot commit the future
    if (!drop) return;
    cur_.mono16k.erase(cur_.mono16k.begin(), cur_.mono16k.begin() + (std::ptrdiff_t)drop);
    commitOff_ += drop;
    // The next snapshot is due a cadence after the SHORTENED buffer, not after the old
    // one: re-arming from here is what keeps the live line's rhythm steady across a
    // commit instead of going quiet for as long as the sentence that just left.
    const size_t cadence = (size_t)partialSamples_.load(std::memory_order_relaxed);
    nextPartialAt_ = cadence ? cur_.mono16k.size() + cadence : (size_t)-1;
    // ...and the pending draft describes audio that is now partly on screen. Dropping it
    // costs one snapshot; keeping it would cost a PART that repaints a finished sentence.
    dropPartial();
}

void TrAudio::commitUpTo(uint64_t segId, size_t sampleOffset) {
    if (!segId) return;
    std::lock_guard<std::mutex> lk(cmx_);
    // LATEST WINS, like the partial slot: a request the audio thread has not picked up yet
    // is strictly superseded by a newer one for the same segment. A request for a DIFFERENT
    // segment replaces it too -- the old one names a segment that has since closed, so
    // applying it would be a no-op anyway.
    if (creqSeg_ == segId && sampleOffset <= creqOff_) return;
    creqSeg_ = segId;
    creqOff_ = sampleOffset;
}

void TrAudio::vadCut(bool forced) {
    energySinceOpen(cur_.srcEnergy);
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) cur_.srcName[k] = curNames_[k];
    cur_.cutTickMs      = ::GetTickCount64();
    cur_.startOffset    = commitOff_;    // what the pipeline has already finalised
    cur_.segStartTickMs = curOpenTickMs_;
    cur_.closes         = true;          // a queued segment is always the end of its utterance
    pushSegment(std::move(cur_));
    // ORDER MATTERS: the final is in the queue BEFORE the live line for it is thrown
    // away, so there is no instant where this utterance has neither. (The pipeline drops
    // it a second time, from its side, the moment it pops that final -- see
    // dropPartialsUpTo. Two owners, because either one alone leaves a window.)
    dropPartial();
    cur_ = Segment{};
    open_ = false;
    commitOff_ = 0;
    hangoverHops_ = 0;
    if (forced) {
        // A forced cut lands mid-sentence. Restart IMMEDIATELY (no pre-roll gap) so the
        // rest of the sentence becomes the next segment instead of waiting for a new
        // attack that will never come -- the speaker never stopped talking.
        vadOpen();
    }
}

void TrAudio::pushSegment(Segment&& s) {
    {
        std::lock_guard<std::mutex> lk(qmx_);
        while (queue_.size() >= kQueueCap) {
            queue_.pop_front();                      // drop the OLDEST: a stale subtitle is
            dropped_.fetch_add(1, std::memory_order_relaxed);   // worse than a missing one
        }
        queue_.push_back(std::move(s));
    }
    qcv_.notify_one();
}

bool TrAudio::pop(Segment& out, uint32_t ms) {
    std::unique_lock<std::mutex> lk(qmx_);
    qcv_.wait_for(lk, std::chrono::milliseconds(ms),
                  [&] { return !queue_.empty() || stop_.load(std::memory_order_relaxed); });
    if (queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

// =============================================================================
// The LIVE LINE tap.
//
// Everything here is deliberately small. The expensive half of progressive display
// lives on the pipeline thread (an ASR call per snapshot); this side's only job is
// to hand it the most recent audio and then get out of the way -- which is why the
// slot is ONE deep, latest-wins, behind its own mutex (see TrAudio.h).
// =============================================================================
void TrAudio::setPartialSec(double sec) {
    uint32_t n = 0;
    if (sec > 0.0) {
        // Clamped rather than rejected: a hand-edited PARTIALSEC of 0.05 would ask for
        // twenty ASR calls a second and read as a broken feature, and one of 60 would
        // read as the feature being absent. 0 (or negative) still means OFF -- that is
        // a choice, not a typo, so it is honoured exactly.
        if (sec < kPartialSecMin) sec = kPartialSecMin;
        if (sec > kPartialSecMax) sec = kPartialSecMax;
        n = (uint32_t)(sec * (double)kAsrRate);
    }
    partialSamples_.store(n, std::memory_order_relaxed);
}

void TrAudio::maybePublishPartial() {
    const size_t cadence = (size_t)partialSamples_.load(std::memory_order_relaxed);
    if (!cadence || !open_) return;
    if (nextPartialAt_ == (size_t)-1) {
        // Turned on while this segment was already open. Arm from HERE rather than
        // firing immediately: the first snapshot of a live line should still be worth
        // a call, not whatever happened to be in the buffer at the moment of the switch.
        nextPartialAt_ = cur_.mono16k.size() + cadence;
        return;
    }
    if (cur_.mono16k.size() < nextPartialAt_) return;

    // The snapshot reaches back to the LAST COMMIT (cur_ holds nothing older), or
    // kPartialMaxMs, whichever is shorter. kMaxSegMs already bounds the uncommitted part
    // at 8 s, so today the cap never binds -- it is here so that ONE constant in this file
    // bounds the cost of one partial inference, whatever a future VAD decides a segment
    // may be.
    const size_t cap  = (size_t)kPartialMaxMs * (size_t)kAsrRate / 1000;
    const size_t n    = cur_.mono16k.size();
    const size_t from = (n > cap) ? (n - cap) : 0;
    pscratch_.assign(cur_.mono16k.begin() + (std::ptrdiff_t)from, cur_.mono16k.end());

    // Who is talking, by the cut's own arithmetic, so that a sentence finalised from this
    // snapshot carries the same input as the closing line of the same breath.
    double e[kMaxLiveInputs];
    energySinceOpen(e);
    int best = -1;
    double bestE = 0.0;
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k)
        if (!curNames_[k].empty() && e[k] > bestE) { bestE = e[k]; best = (int)k; }

    {
        std::lock_guard<std::mutex> lk(pmx_);
        // The one already in there was never picked up. That is the DESIGN, not a
        // fault: it has just been superseded by a strictly better snapshot of the same
        // sentence. Count it, so the cadence claim can be checked instead of believed.
        if (pendHas_) pSkipped_.fetch_add(1, std::memory_order_relaxed);
        pend_.segId          = cur_.segId;
        pend_.tickMs         = ::GetTickCount64();
        pend_.peakDbfs       = cur_.peakDbfs;
        pend_.startOffset    = commitOff_ + from;
        pend_.segStartTickMs = curOpenTickMs_;
        pend_.srcSlot        = best < 0 ? std::string() : curNames_[(size_t)best];
        pend_.mono16k.swap(pscratch_);      // O(1): the copy above happened OUTSIDE the lock
        pendHas_ = true;
    }
    pMade_.fetch_add(1, std::memory_order_relaxed);
    nextPartialAt_ = n + cadence;
}

void TrAudio::dropPartial() {
    std::lock_guard<std::mutex> lk(pmx_);
    pendHas_ = false;
    pend_.segId = 0;
}

bool TrAudio::popPartial(PartialSnap& out) {
    std::lock_guard<std::mutex> lk(pmx_);
    if (!pendHas_) return false;
    out.segId          = pend_.segId;
    out.tickMs         = pend_.tickMs;
    out.peakDbfs       = pend_.peakDbfs;
    out.startOffset    = pend_.startOffset;
    out.segStartTickMs = pend_.segStartTickMs;
    out.srcSlot        = pend_.srcSlot;
    out.mono16k.swap(pend_.mono16k);   // the caller's old buffer goes back to the audio side
    pend_.mono16k.clear();             // ... keeping its capacity for the next swap
    pendHas_ = false;
    return true;
}

void TrAudio::dropPartialsUpTo(uint64_t segId) {
    std::lock_guard<std::mutex> lk(pmx_);
    if (pendHas_ && pend_.segId <= segId) pendHas_ = false;
}

bool TrAudio::finalWaiting() const {
    std::lock_guard<std::mutex> lk(qmx_);
    return !queue_.empty();
}

void TrAudio::clear() {
    {
        std::lock_guard<std::mutex> lk(qmx_);
        queue_.clear();
    }
    // The half-cut segment is AUDIO-THREAD state. Raise a flag; that thread drops it on
    // its next pass. (Reaching in from here and writing open_/cur_ directly is a data
    // race for the sake of ~2 ms, which is not a trade worth making.)
    clearAsked_.store(true, std::memory_order_release);
}

// =============================================================================
// SELFTEST path -- the same VAD and queue, fed from a file.
// =============================================================================
void TrAudio::selftestBegin(uint32_t inRate, const std::string& fakeSourceName) {
    dec_.init(inRate);
    hopSamples_ = kAsrRate * kVadHopMs / 1000;
    hop_.clear();
    hop_.reserve((size_t)hopSamples_);
    preRoll_.assign((size_t)((kPreRollMs + kVadHopMs) * kAsrRate / 1000), 0.0f);
    preRollAt_ = 0; preRollFull_ = false;
    accHist_.assign((size_t)kAccHistory, AccSnap{});
    accHead_ = 0; accCount_ = 0;
    for (uint32_t k = 0; k < kMaxLiveInputs; ++k) { accSum_[k] = 0.0; boundName_[k].clear(); }
    accN_ = 0.0;
    boundName_[0] = fakeSourceName;
    open_ = false;
    cur_ = Segment{};
    commitOff_ = 0;
    { std::lock_guard<std::mutex> lk(cmx_); creqSeg_ = 0; creqOff_ = 0; }
    { std::lock_guard<std::mutex> lk(qmx_); queue_.clear(); }
    // The live line starts from zero too, INCLUDING the counters: the selftest asserts
    // on partialsMade(), so carrying a previous file's total into this one would make
    // the second case pass on the first case's evidence.
    nextSegId_ = 0;
    nextPartialAt_ = (size_t)-1;
    pMade_.store(0, std::memory_order_relaxed);
    pSkipped_.store(0, std::memory_order_relaxed);
    dropPartial();
}

void TrAudio::selftestFeed(const float* monoIn, int n) {
    double e = 0.0;
    for (int i = 0; i < n; ++i) e += (double)monoIn[i] * (double)monoIn[i];
    accSum_[0] += e;
    accN_ += (double)n;
    mono16_.clear();
    dec_.process(monoIn, n, mono16_);
    if (!mono16_.empty()) vadPush(mono16_.data(), (int)mono16_.size());
}

void TrAudio::selftestEnd() {
    // Flush: end of file is a close, so any open segment long enough to be a sentence
    // must be emitted rather than silently dropped with the last words in it. The
    // "or something was committed" half is vadPush's rule, verbatim and for its reason:
    // a tail that only closes an utterance is still a message the pipeline owes the UI.
    applyCommit();          // a request the last block asked for still counts
    if (open_ && (cur_.mono16k.size() >= (size_t)(kMinSegMs * kAsrRate / 1000)
                  || commitOff_ > 0)) {
        energySinceOpen(cur_.srcEnergy);
        for (uint32_t k = 0; k < kMaxLiveInputs; ++k) cur_.srcName[k] = curNames_[k];
        cur_.cutTickMs      = ::GetTickCount64();
        cur_.startOffset    = commitOff_;
        cur_.segStartTickMs = curOpenTickMs_;
        cur_.closes         = true;
        pushSegment(std::move(cur_));
    }
    dropPartial();          // same order as vadCut: queue the final, then forget the draft
    cur_ = Segment{};
    commitOff_ = 0;
    open_ = false;
}

} // namespace pbt
