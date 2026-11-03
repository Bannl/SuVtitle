// =============================================================================
// SuVtitle - engine/TrCaptureCore.cpp   (the capture's building blocks -- see TrCaptureCore.h)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b. Clean room: Microsoft's documentation (WAVEFORMATEX / WAVEFORMATEXTENSIBLE)
// and this repository's contract only.
// =============================================================================
#include "TrCaptureCore.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace pbt {
namespace cap {

namespace {

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT (ksmedia.h): spelled out here rather than pulled in through ksmedia.h's
// DEFINE_GUIDSTRUCT machinery -- the values are the documented WAVE_FORMAT tags in the base-GUID pattern.
const GUID kSubtypePcm   = { 0x00000001, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

inline float finiteOr0(float v) { return (v == v && v <= 1e30f && v >= -1e30f) ? v : 0.0f; }

} // namespace

// ---------------------------------------------------------------------------------------------------------------
float peakDbfs(float peak) {
    if (!(peak > 1e-6f)) return -120.0f;
    const double db = 20.0 * std::log10((double)peak);
    return db < -120.0 ? -120.0f : (float)db;
}

double decimatorDelayMs(uint32_t rate) {
    if (!rate) return 0.0;
    const double fcNorm = 0.85 * (kAsrRate * 0.5) / (double)rate;
    int halfTaps = (int)std::ceil(32.0 / (2.0 * fcNorm));
    if (halfTaps < 4)   halfTaps = 4;
    if (halfTaps > 512) halfTaps = 512;
    return (double)halfTaps * 1000.0 / (double)rate;
}

// ---- LaneFifo ---------------------------------------------------------------------------------------------------
LaneFifo::LaneFifo() : buf_(new std::atomic<float>[kFifoCapacity]) {
    for (int i = 0; i < kFifoCapacity; ++i) buf_[i].store(0.0f, std::memory_order_relaxed);
}

void LaneFifo::push(const float* x, int n) {
    if (n <= 0) return;
    if (n > kFifoCapacity) {                     // only the newest kFifoCapacity can ever be kept
        dropped_.fetch_add((uint64_t)(n - kFifoCapacity), std::memory_order_relaxed);
        x += n - kFifoCapacity;
        n = kFifoCapacity;
    }
    const uint64_t w = w_.load(std::memory_order_relaxed);   // ours alone
    // Room first: the samples about to be overwritten are given up BEFORE they are written, so a consumer copying
    // them sees its commit fail and copies again.
    for (;;) {
        uint64_t r = r_.load(std::memory_order_acquire);
        const uint64_t used = w - r;
        if (used + (uint64_t)n <= (uint64_t)kFifoCapacity) break;
        const uint64_t need = used + (uint64_t)n - (uint64_t)kFifoCapacity;
        if (r_.compare_exchange_weak(r, r + need, std::memory_order_acq_rel, std::memory_order_acquire)) {
            dropped_.fetch_add(need, std::memory_order_relaxed);
            break;
        }
    }
    for (int i = 0; i < n; ++i) buf_[(size_t)((w + (uint64_t)i) & kMask)].store(x[i], std::memory_order_relaxed);
    w_.store(w + (uint64_t)n, std::memory_order_release);
}

int LaneFifo::size() const {
    const uint64_t r = r_.load(std::memory_order_acquire);
    const uint64_t w = w_.load(std::memory_order_acquire);
    const uint64_t d = w - r;
    return d > (uint64_t)kFifoCapacity ? kFifoCapacity : (int)d;
}

int LaneFifo::read(float* out, int n) {
    if (n <= 0) return 0;
    for (;;) {
        uint64_t r = r_.load(std::memory_order_acquire);
        const uint64_t w = w_.load(std::memory_order_acquire);
        if (w - r > (uint64_t)kFifoCapacity) continue;          // the producer dropped since we read r: reload
        const int m = (int)std::min<uint64_t>((uint64_t)n, w - r);
        for (int i = 0; i < m; ++i) out[i] = buf_[(size_t)((r + (uint64_t)i) & kMask)].load(std::memory_order_relaxed);
        if (r_.compare_exchange_strong(r, r + (uint64_t)m, std::memory_order_acq_rel, std::memory_order_acquire))
            return m;
        // The producer moved the read index (it dropped the oldest -- possibly what we just copied): again.
    }
}

int LaneFifo::skip(int n) {
    if (n <= 0) return 0;
    for (;;) {
        uint64_t r = r_.load(std::memory_order_acquire);
        const uint64_t w = w_.load(std::memory_order_acquire);
        if (w - r > (uint64_t)kFifoCapacity) continue;
        const int m = (int)std::min<uint64_t>((uint64_t)n, w - r);
        if (r_.compare_exchange_strong(r, r + (uint64_t)m, std::memory_order_acq_rel, std::memory_order_acquire))
            return m;
    }
}

// ---- Lane -------------------------------------------------------------------------------------------------------
Lane::Lane(const LiveInput& in, const std::string& k) : key(in.key), chan(in.chan), kind(k) {
    setGainDb(in.gainDb);
}

void Lane::setGainDb(double db) {
    gainDb_.store(db, std::memory_order_relaxed);
    gain_.store(linFromDb(db), std::memory_order_relaxed);
}

bool Lane::produce(const float* x, uint32_t frames, uint32_t channels, uint32_t rate) {
    if (!frames || !channels || rate < (uint32_t)kAsrRate) return true;
    mono_.resize(frames);
    const float g = gain_.load(std::memory_order_relaxed);
    bool have = true;
    if (chan == 0) {
        // The SUM of the channels, not their mean: AudioPB's mono rule, on which the VAD's -38 dBFS gate was
        // calibrated (a voice centred in stereo reads +6 dB, as it did there -- TrPipe.h, LIVE).
        for (uint32_t f = 0; f < frames; ++f) {
            const float* fr = x + (size_t)f * channels;
            float s = 0.0f;
            for (uint32_t c = 0; c < channels; ++c) s += finiteOr0(fr[c]);
            mono_[f] = guardSample(s * g);
        }
    } else if (chan <= channels) {
        const uint32_t c = chan - 1;
        for (uint32_t f = 0; f < frames; ++f) mono_[f] = guardSample(finiteOr0(x[(size_t)f * channels + c]) * g);
    } else {
        std::fill(mono_.begin(), mono_.end(), 0.0f);
        have = false;
    }
    if (rate != decRate_) {                      // first packet, or the device came back at another rate
        dec_.init(rate);
        decRate_ = rate;
        decimatorMs_.store(decimatorDelayMs(rate), std::memory_order_relaxed);
    }
    out16_.clear();
    dec_.process(mono_.data(), (int)frames, out16_);
    // The Decimator's sinc can ring a hair past a clamped input: the contract's +-4 holds AFTER it too.
    for (float& v : out16_) v = guardSample(v);
    fifo.push(out16_.data(), (int)out16_.size());

    const double pkMs = (double)frames * 1000.0 / (double)rate;
    const double was = periodMs_.load(std::memory_order_relaxed);
    periodMs_.store(was == 0.0 ? pkMs : was + (pkMs - was) * 0.05, std::memory_order_relaxed);
    const int pk16 = (int)((uint64_t)frames * (uint64_t)kAsrRate / rate);
    if (pk16 > maxPacket16k_.load(std::memory_order_relaxed)) maxPacket16k_.store(pk16, std::memory_order_relaxed);
    return have;
}

void Lane::produceSilence(uint32_t frames, uint32_t rate) {
    if (!frames || rate < (uint32_t)kAsrRate) return;
    mono_.assign(frames, 0.0f);
    if (rate != decRate_) {
        dec_.init(rate);
        decRate_ = rate;
        decimatorMs_.store(decimatorDelayMs(rate), std::memory_order_relaxed);
    }
    out16_.clear();
    dec_.process(mono_.data(), (int)frames, out16_);
    for (float& v : out16_) v = guardSample(v);
    fifo.push(out16_.data(), (int)out16_.size());
    const double pkMs = (double)frames * 1000.0 / (double)rate;
    const double was = periodMs_.load(std::memory_order_relaxed);
    periodMs_.store(was == 0.0 ? pkMs : was + (pkMs - was) * 0.05, std::memory_order_relaxed);
}

int Lane::target() const {
    const int base = (kind == "out" || kind == "app") ? kTargetLoopFrames : kTargetInFrames;
    // A source that hands over big packets needs room for one and a half of them plus the mixer's own block, or
    // the lane runs dry between two packets. Capped: one odd packet must not cost a quarter second for good.
    int big = maxPacket16k_.load(std::memory_order_relaxed);
    big = std::min(big, kAsrRate / 4);
    return std::max(base, big + big / 2 + kFeedMaxFrames);
}

void Lane::pull(float* out, int n) {
    const int T = target();
    int avail = fifo.size();
    if (!primed_) {
        if (avail < T) {
            // Not enough in front yet (start, or after running dry): silence, and nothing taken, so the fill grows
            // to the target before the lane speaks again.
            std::fill(out, out + n, 0.0f);
            fedFrames_.fetch_add((uint64_t)n, std::memory_order_relaxed);
            return;
        }
        primed_ = true;
        ema_ = (double)avail;
        sinceSlip_ = 0;
        highRun_ = 0;
        primedPub_.store(true, std::memory_order_relaxed);
    }
    if (avail < n) {
        // Ran dry (the source stopped: unplugged, a loopback with nothing playing, a stall): what is left, then
        // zeros -- the VAD must keep seeing time pass -- and back to priming.
        const int got = fifo.read(out, avail);
        std::fill(out + got, out + n, 0.0f);
        primed_ = false;
        primedPub_.store(false, std::memory_order_relaxed);
        underruns_.fetch_add(1, std::memory_order_relaxed);
        fedFrames_.fetch_add((uint64_t)n, std::memory_order_relaxed);
        return;
    }
    if (avail > T + kResyncFrames) {
        if (++highRun_ >= kResyncBlocks) {
            fifo.skip(avail - T);
            resyncs_.fetch_add(1, std::memory_order_relaxed);
            highRun_ = 0;
            avail = fifo.size();
            ema_ = (double)avail;
        }
    } else {
        highRun_ = 0;
    }
    ema_ += ((double)avail - ema_) * kFillAlpha;

    int take = n;
    if (++sinceSlip_ >= kSlipEveryBlocks) {
        if (ema_ > (double)(T + kSlipBandFrames) && avail >= n + 1) take = n + 1;
        else if (ema_ < (double)(T - kSlipBandFrames) && n >= 2)   take = n - 1;
    }
    if (take == n) {
        const int got = fifo.read(out, n);
        if (got < n) std::fill(out + got, out + n, 0.0f);   // only if the producer dropped in between (full FIFO)
    } else if (take == n + 1) {
        // The source runs fast: ONE sample dropped (the block's last), 62.5 us -- inaudible to a recogniser.
        tmp_.resize((size_t)n + 1);
        const int got = fifo.read(tmp_.data(), n + 1);
        std::copy(tmp_.begin(), tmp_.begin() + std::min(got, n), out);
        if (got < n) std::fill(out + got, out + n, 0.0f);
        slipsUp_.fetch_add(1, std::memory_order_relaxed);
        ema_ -= 1.0;
        sinceSlip_ = 0;
    } else {
        // The source runs slow: ONE sample repeated.
        const int got = fifo.read(out, n - 1);
        if (got < n - 1) std::fill(out + got, out + n - 1, 0.0f);
        out[n - 1] = out[n - 2];
        slipsDown_.fetch_add(1, std::memory_order_relaxed);
        ema_ += 1.0;
        sinceSlip_ = 0;
    }
    fedFrames_.fetch_add((uint64_t)n, std::memory_order_relaxed);
    fillMs_.store(ema_ * 1000.0 / (double)kAsrRate, std::memory_order_relaxed);
}

Lane::Stats Lane::stats() const {
    Stats s;
    s.fedFrames   = fedFrames_.load(std::memory_order_relaxed);
    s.dropped     = fifo.dropped();
    s.underruns   = underruns_.load(std::memory_order_relaxed);
    s.slipsUp     = slipsUp_.load(std::memory_order_relaxed);
    s.slipsDown   = slipsDown_.load(std::memory_order_relaxed);
    s.resyncs     = resyncs_.load(std::memory_order_relaxed);
    s.fillMs      = fillMs_.load(std::memory_order_relaxed);
    s.targetMs    = (double)target() * 1000.0 / (double)kAsrRate;
    s.periodMs    = periodMs_.load(std::memory_order_relaxed);
    s.decimatorMs = decimatorMs_.load(std::memory_order_relaxed);
    s.primed      = primedPub_.load(std::memory_order_relaxed);
    return s;
}

double Lane::delayMs() const {
    const Stats s = stats();
    if (s.periodMs <= 0.0) return 0.0;
    return s.periodMs + (s.primed ? s.fillMs : s.targetMs) + s.decimatorMs;
}

// ---- DeviceFeed -------------------------------------------------------------------------------------------------
DeviceFeed::DeviceFeed(std::function<void(const std::string&)> log) : log_(std::move(log)) {}

void DeviceFeed::setLanes(std::vector<std::shared_ptr<Lane>> lanes) {
    std::lock_guard<std::mutex> lk(mu_);
    lanes_ = std::move(lanes);
}

std::vector<std::shared_ptr<Lane>> DeviceFeed::lanes() const {
    std::lock_guard<std::mutex> lk(mu_);
    return lanes_;
}

void DeviceFeed::push(const float* x, uint32_t frames, uint32_t channels, uint32_t rate) {
    const std::vector<std::shared_ptr<Lane>> ls = lanes();
    // forget the warnings of lanes that are gone (a later lane may reuse the address)
    warned_.erase(std::remove_if(warned_.begin(), warned_.end(), [&](const Lane* w) {
        for (const auto& l : ls) if (l.get() == w) return false;
        return true;
    }), warned_.end());
    for (const auto& l : ls) {
        if (l->produce(x, frames, channels, rate)) continue;
        if (std::find(warned_.begin(), warned_.end(), l.get()) != warned_.end()) continue;
        warned_.push_back(l.get());
        if (log_) {
            char b[160];
            std::snprintf(b, sizeof(b), ": channel %u does not exist on this source (it has %u); the strip carries "
                          "silence", (unsigned)l->chan, (unsigned)channels);
            log_("capture: " + l->key + b);
        }
    }
}

void DeviceFeed::pushSilence(uint32_t frames, uint32_t channels, uint32_t rate) {
    (void)channels;
    for (const auto& l : lanes()) l->produceSilence(frames, rate);
}

// ---- BoundedThread ----------------------------------------------------------------------------------------------
struct BoundedThread::Done {
    HANDLE ev = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ~Done() { if (ev) ::CloseHandle(ev); }
};

BoundedThread::~BoundedThread() {
    if (th_.joinable()) th_.detach();
}

void BoundedThread::start(std::function<void()> body) {
    done_ = std::make_shared<Done>();
    std::shared_ptr<Done> d = done_;           // the thread's own reference: the event outlives an abandonment
    th_ = std::thread([body, d]() {
        body();
        if (d->ev) ::SetEvent(d->ev);
    });
}

bool BoundedThread::join(DWORD ms) {
    if (!th_.joinable()) return true;
    if (th_.get_id() == std::this_thread::get_id()) { th_.detach(); return true; }
    if (done_ && done_->ev && ::WaitForSingleObject(done_->ev, ms) != WAIT_OBJECT_0) {
        th_.detach();
        return false;
    }
    th_.join();
    return true;
}

// ---- EventGate / StateReporter ----------------------------------------------------------------------------------
namespace {
// The Defer scope of THIS thread (start() / stop() holding the hub's lock), or null.
thread_local std::vector<std::function<void()>>* t_deferred = nullptr;
}

EventGate::Defer::Defer() : prev_(t_deferred) { t_deferred = &q_; }

EventGate::Defer::~Defer() {
    t_deferred = prev_;
    std::vector<std::function<void()>> q;
    q.swap(q_);
    for (auto& f : q) f();
}

void EventGate::route(std::function<void(ILiveEvents&)> f) {
    if (t_deferred) {
        std::shared_ptr<EventGate> self = shared_from_this();
        t_deferred->push_back([self, f]() { self->deliver(f); });
        return;
    }
    deliver(f);
}

void EventGate::deliver(const std::function<void(ILiveEvents&)>& f) {
    inflight_.fetch_add(1);
    if (!closed_.load() && events_) {
        try { f(*events_); } catch (...) {}
    }
    inflight_.fetch_sub(1);
}

void EventGate::log(const std::string& line) {
    route([line](ILiveEvents& e) { e.onLog(line); });
}

void EventGate::inputState(const std::string& key, const std::string& state, const std::string& why) {
    route([key, state, why](ILiveEvents& e) { e.onInputState(key, state, why); });
}

void EventGate::levels(const std::vector<std::pair<std::string, float>>& peaks) {
    route([peaks](ILiveEvents& e) { e.onLevels(peaks); });
}

void EventGate::sources(const std::vector<SourceInfo>& list) {
    route([list](ILiveEvents& e) { e.onSources(list); });
}

void EventGate::close() {
    closed_.store(true);
    while (inflight_.load() != 0) ::Sleep(1);
}

void StateReporter::set(const std::string& state, const std::string& why) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (state == state_) return;
        state_ = state;
    }
    gate_->inputState(key_, state, why);
}

std::string StateReporter::state() const {
    std::lock_guard<std::mutex> lk(mu_);
    return state_;
}

// ---- RetryPolicy (fix round RVA-8) ------------------------------------------------------------------------------
uint32_t RetryPolicy::ladderMs(size_t step) {
    static const uint32_t kLadder[] = { 200, 1000, 2000, 5000 };
    return kLadder[std::min(step, sizeof(kLadder) / sizeof(kLadder[0]) - 1)];
}

bool RetryPolicy::opened(uint64_t nowMs) {
    running_ = true;
    openedAt_ = nowMs;
    held_ = flapping_;
    return !held_;
}

bool RetryPolicy::healthy(uint64_t nowMs, bool& reportOpen) {
    reportOpen = false;
    if (!running_ || !episode_ || nowMs - openedAt_ < kHealthyMs) return false;
    reportOpen = held_;
    episode_ = flapping_ = held_ = false;
    step_ = 0;
    tries_ = 0;
    lastKind_.clear();
    return true;
}

RetryPolicy::Failure RetryPolicy::failed(uint64_t nowMs, const std::string& kind, uint32_t retryMs) {
    if (running_) {
        if (nowMs - openedAt_ < kHealthyMs) {
            if (episode_) flapping_ = true;            // it reopened, then failed at once: a flap
        } else {
            // It ran healthy (and healthy() was not asked in time): a NEW episode, from the ladder's first step.
            episode_ = flapping_ = false;
            step_ = 0;
            tries_ = 0;
            lastKind_.clear();
        }
    }
    running_ = held_ = false;
    episode_ = true;
    Failure f;
    f.tries = ++tries_;
    f.waitMs = retryMs ? retryMs : ladderMs(step_);
    ++step_;
    if (kind != lastKind_) {
        f.log = true;
        lastKind_ = kind;
        lastLogAt_ = nowMs;
    } else if (nowMs - lastLogAt_ >= kStillLogMs) {
        f.still = true;
        lastLogAt_ = nowMs;
    }
    return f;
}

// ---- formats ----------------------------------------------------------------------------------------------------
bool parseFormat(const WAVEFORMATEX* wfx, size_t cb, SampleFormat& out, std::string& why) {
    out = SampleFormat{};
    if (!wfx || cb < 16) { why = "no usable format description"; return false; }   // PCMWAVEFORMAT = 16 bytes
    WORD tag = wfx->wFormatTag;
    const WORD bits = wfx->wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (cb < sizeof(WAVEFORMATEXTENSIBLE) || wfx->cbSize < 22) { why = "a truncated extensible format"; return false; }
        const WAVEFORMATEXTENSIBLE* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (x->SubFormat == kSubtypeFloat)    tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (x->SubFormat == kSubtypePcm) tag = WAVE_FORMAT_PCM;
        else { why = "a compressed or vendor format"; return false; }
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (bits == 32)      out.kind = SampleKind::F32;
        else if (bits == 64) out.kind = SampleKind::F64;
    } else if (tag == WAVE_FORMAT_PCM) {
        if (bits == 8)       out.kind = SampleKind::U8;
        else if (bits == 16) out.kind = SampleKind::I16;
        else if (bits == 24) out.kind = SampleKind::I24;
        else if (bits == 32) out.kind = SampleKind::I32;
    } else {
        why = "a compressed format";
        return false;
    }
    if (out.kind == SampleKind::Unknown) { why = std::to_string(bits) + "-bit samples"; return false; }
    out.rate = wfx->nSamplesPerSec;
    out.channels = wfx->nChannels;
    out.containerBytes = bits / 8u;
    out.blockAlign = wfx->nBlockAlign;
    if (out.channels < 1 || out.channels > 64) { why = std::to_string(out.channels) + " channels"; return false; }
    if (out.rate < 1000 || out.rate > 1000000) { why = std::to_string(out.rate) + " Hz"; return false; }
    if (out.blockAlign < out.channels * out.containerBytes) { why = "an inconsistent block size"; return false; }
    return true;
}

void toFloat(const unsigned char* data, uint32_t frames, const SampleFormat& f, float* out) {
    const size_t ch = f.channels;
    for (uint32_t fr = 0; fr < frames; ++fr) {
        const unsigned char* p = data + (size_t)fr * f.blockAlign;
        float* o = out + (size_t)fr * ch;
        for (size_t c = 0; c < ch; ++c, p += f.containerBytes) {
            switch (f.kind) {
            case SampleKind::F32: { float v; std::memcpy(&v, p, 4); o[c] = v; break; }
            case SampleKind::F64: { double v; std::memcpy(&v, p, 8); o[c] = (float)v; break; }
            case SampleKind::I16: { int16_t v; std::memcpy(&v, p, 2); o[c] = (float)v / 32768.0f; break; }
            case SampleKind::I24: {
                const int32_t v = (int32_t)(((uint32_t)p[2] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[0] << 8)) >> 8;
                o[c] = (float)v / 8388608.0f;
                break;
            }
            case SampleKind::I32: { int32_t v; std::memcpy(&v, p, 4); o[c] = (float)((double)v / 2147483648.0); break; }
            case SampleKind::U8:  o[c] = ((float)p[0] - 128.0f) / 128.0f; break;
            default:              o[c] = 0.0f; break;
            }
        }
    }
}

std::string describe(const SampleFormat& f) {
    const char* k = "?";
    switch (f.kind) {
    case SampleKind::F32: k = "float32"; break;
    case SampleKind::F64: k = "float64"; break;
    case SampleKind::I16: k = "pcm16"; break;
    case SampleKind::I24: k = "pcm24"; break;
    case SampleKind::I32: k = "pcm32"; break;
    case SampleKind::U8:  k = "pcm8"; break;
    default: break;
    }
    char b[96];
    std::snprintf(b, sizeof(b), "%u Hz, %u ch, %s", (unsigned)f.rate, (unsigned)f.channels, k);
    return b;
}

WAVEFORMATEXTENSIBLE floatFormat(uint32_t rate, uint32_t channels) {
    WAVEFORMATEXTENSIBLE x = {};
    x.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    x.Format.nChannels = (WORD)channels;
    x.Format.nSamplesPerSec = rate;
    x.Format.wBitsPerSample = 32;
    x.Format.nBlockAlign = (WORD)(channels * 4u);
    x.Format.nAvgBytesPerSec = rate * x.Format.nBlockAlign;
    x.Format.cbSize = (WORD)(sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    x.Samples.wValidBitsPerSample = 32;
    // Mono and stereo get their usual speakers; more channels are left unassigned (0): the capture only ever picks
    // or sums channels by NUMBER, never by speaker position.
    x.dwChannelMask = channels == 1 ? SPEAKER_FRONT_CENTER : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0;
    x.SubFormat = kSubtypeFloat;
    return x;
}

WAVEFORMATEX pcm16Format(uint32_t rate, uint32_t channels) {
    WAVEFORMATEX w = {};
    w.wFormatTag = WAVE_FORMAT_PCM;
    w.nChannels = (WORD)channels;
    w.nSamplesPerSec = rate;
    w.wBitsPerSample = 16;
    w.nBlockAlign = (WORD)(channels * 2u);
    w.nAvgBytesPerSec = rate * w.nBlockAlign;
    w.cbSize = 0;
    return w;
}

// ---- helpers ----------------------------------------------------------------------------------------------------
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string hr8(long hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

std::string fmtMs(double ms) {
    char b[32];
    std::snprintf(b, sizeof(b), "%.1f", ms);
    return b;
}

} // namespace cap
} // namespace pbt
