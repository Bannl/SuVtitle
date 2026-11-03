// =============================================================================
// SuVtitle - engine/TrCaptureWav.cpp   (the "wav:" TEST source -- see TrCaptureWav.h)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b.
// =============================================================================
#include "TrCaptureWav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace pbt {
namespace cap {

namespace {

// An input that sends nothing for this long is reported stalled (the WASAPI input source keeps the same rule).
constexpr double kStallSec = 2.0;

bool startsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

bool parseDouble(const std::string& s, double& v) {
    if (s.empty()) return false;
    char* end = nullptr;
    v = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && v == v;
}

bool parseSpan(const std::string& s, std::pair<double, double>& span) {
    const size_t c = s.find(':');
    if (c == std::string::npos) return false;
    double t = 0, d = 0;
    if (!parseDouble(s.substr(0, c), t) || !parseDouble(s.substr(c + 1), d)) return false;
    if (t < 0 || d <= 0 || t > 1e6 || d > 1e6) return false;
    span = { t, d };
    return true;
}

bool inSpans(const std::vector<std::pair<double, double>>& v, double t) {
    for (const auto& s : v) if (t >= s.first && t < s.first + s.second) return true;
    return false;
}

bool readWholeFile(const std::wstring& path, std::vector<unsigned char>& data, std::string& why) {
    HANDLE f = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { why = "cannot open it (" + hr8(HRESULT_FROM_WIN32(::GetLastError())) + ")"; return false; }
    LARGE_INTEGER sz = {};
    bool ok = ::GetFileSizeEx(f, &sz) != 0;
    if (ok && (sz.QuadPart < 12 || sz.QuadPart > (LONGLONG)1024 * 1024 * 1024)) { why = "not a usable size"; ok = false; }
    if (ok) {
        data.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (ok && got < data.size()) {
            const DWORD want = (DWORD)std::min<size_t>(data.size() - got, (size_t)1 << 24);
            DWORD n = 0;
            if (!::ReadFile(f, data.data() + got, want, &n, nullptr) || n == 0) { why = "a read failed"; ok = false; }
            got += n;
        }
    }
    ::CloseHandle(f);
    return ok;
}

// A test signal, not a master: linear interpolation is plenty to put a file at the "device" rate a test asks for.
std::vector<float> resampleLinear(const std::vector<float>& in, uint32_t ch, uint64_t frames, uint32_t from, uint32_t to) {
    const uint64_t outFrames = frames * (uint64_t)to / (uint64_t)from;
    std::vector<float> out((size_t)(outFrames * ch));
    const double step = (double)from / (double)to;
    for (uint64_t j = 0; j < outFrames; ++j) {
        const double pos = (double)j * step;
        uint64_t i = (uint64_t)pos;
        const double fr = pos - (double)i;
        if (i >= frames) i = frames - 1;
        const uint64_t i1 = (i + 1 < frames) ? i + 1 : i;
        for (uint32_t c = 0; c < ch; ++c)
            out[(size_t)(j * ch + c)] = (float)((double)in[(size_t)(i * ch + c)] * (1.0 - fr) + (double)in[(size_t)(i1 * ch + c)] * fr);
    }
    return out;
}

} // namespace

bool parseWavKey(const std::string& key, WavOptions& o, std::string& why) {
    o = WavOptions{};
    if (!startsWith(key, "wav:")) { why = "not a wav: key"; return false; }
    std::string rest = key.substr(4);
    for (;;) {
        const size_t at = rest.rfind('@');
        if (at == std::string::npos) break;
        const std::string opt = rest.substr(at + 1);
        if (opt == "loop") {
            o.loop = true;
        } else if (startsWith(opt, "rate=")) {
            double r = 0;
            if (!parseDouble(opt.substr(5), r) || r < 1000 || r > 768000 || r != std::floor(r)) { why = "bad @rate"; return false; }
            o.rate = (uint32_t)r;
        } else if (startsWith(opt, "drift=")) {
            std::string v = opt.substr(6);
            if (v.size() > 3 && v.compare(v.size() - 3, 3, "ppm") == 0) v.resize(v.size() - 3);
            if (!v.empty() && v[0] == '+') v.erase(0, 1);
            double d = 0;
            if (!parseDouble(v, d) || d < -100000 || d > 100000) { why = "bad @drift"; return false; }
            o.driftPpm = d;
        } else if (startsWith(opt, "gap=")) {
            std::pair<double, double> s;
            if (!parseSpan(opt.substr(4), s)) { why = "bad @gap"; return false; }
            o.gaps.push_back(s);
        } else if (startsWith(opt, "flap=")) {
            std::pair<double, double> s;
            if (!parseSpan(opt.substr(5), s)) { why = "bad @flap"; return false; }
            o.flaps.push_back(s);
        } else if (startsWith(opt, "stall=")) {
            std::pair<double, double> s;
            if (!parseSpan(opt.substr(6), s)) { why = "bad @stall"; return false; }
            o.stalls.push_back(s);
        } else {
            break;                                   // not one of ours: the '@' belongs to the path
        }
        rest.resize(at);
    }
    if (rest.empty()) { why = "no file path"; return false; }
    o.path = rest;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
WavSource::WavSource(const std::string& key, std::shared_ptr<EventGate> gate, bool virtualClock)
    : Source(key, std::make_shared<DeviceFeed>([gate](const std::string& l) { gate->log(l); })),
      gate_(gate), reporter_(key, gate), virtual_(virtualClock) {
    stopEv_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    firstEv_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WavSource::~WavSource() {
    if (stopEv_) ::CloseHandle(stopEv_);
    if (firstEv_) ::CloseHandle(firstEv_);
}

bool WavSource::waitFirstOpen(DWORD ms) { return ::WaitForSingleObject(firstEv_, ms) == WAIT_OBJECT_0; }

bool WavSource::open() {
    std::string why;
    std::vector<unsigned char> data;
    SampleFormat fmt;
    uint32_t fileRate = 0;
    ok_ = false;
    if (!parseWavKey(key_, opt_, why)) {
        why = "its key is malformed (" + why + ")";
    } else if (readWholeFile(wide(opt_.path), data, why)) {
        if (std::memcmp(data.data(), "RIFF", 4) != 0 || std::memcmp(data.data() + 8, "WAVE", 4) != 0) {
            why = "not a RIFF/WAVE file";
        } else {
            const unsigned char* fmtp = nullptr;
            const unsigned char* pcm = nullptr;
            size_t fmtLen = 0, pcmLen = 0, at = 12;
            while (at + 8 <= data.size()) {
                uint32_t len = 0;
                std::memcpy(&len, data.data() + at + 4, 4);
                const size_t body = at + 8;
                const size_t avail = std::min<size_t>(len, data.size() - body);
                if (std::memcmp(data.data() + at, "fmt ", 4) == 0)      { fmtp = data.data() + body; fmtLen = avail; }
                else if (std::memcmp(data.data() + at, "data", 4) == 0) { pcm = data.data() + body; pcmLen = avail; }
                const size_t next = body + (size_t)len + (len & 1u);
                if (next <= at) break;
                at = next;
            }
            WAVEFORMATEXTENSIBLE fx = {};
            if (fmtp) std::memcpy(&fx, fmtp, std::min(fmtLen, sizeof(fx)));
            if (!fmtp || !pcm) {
                why = "no fmt or data chunk";
            } else if (parseFormat(reinterpret_cast<const WAVEFORMATEX*>(&fx), fmtLen, fmt, why)) {
                frames_ = pcmLen / fmt.blockAlign;
                ch_ = fmt.channels;
                fileRate = fmt.rate;
                pcm_.resize((size_t)(frames_ * ch_));
                toFloat(pcm, (uint32_t)frames_, fmt, pcm_.data());
                rate_ = fileRate;
                if (opt_.rate && opt_.rate != fileRate && frames_ > 0) {
                    pcm_ = resampleLinear(pcm_, ch_, frames_, fileRate, opt_.rate);
                    frames_ = pcm_.size() / ch_;
                    rate_ = opt_.rate;
                }
                if (rate_ < (uint32_t)kAsrRate) why = "it runs below 16 kHz, which the capture never decimates (" + std::to_string(rate_) + " Hz)";
                else if (frames_ == 0)          why = "it holds no audio";
                else                            ok_ = true;
            } else {
                why = "its format is " + why;
            }
        }
    }
    if (!ok_) {
        gate_->log("capture: " + key_ + ": the test file could not be used: " + why);
        reporter_.set("error", "The test file could not be used: " + why + ".");
        return false;
    }
    packet_ = std::max<uint32_t>(1, rate_ / 100);
    pkt_.assign((size_t)packet_ * ch_, 0.0f);
    char b[256];
    std::snprintf(b, sizeof(b), ": %.2f s", (double)frames_ / (double)rate_);
    std::string line = "capture: opened " + key_ + ": " + describe(SampleFormat{ rate_, ch_, ch_ * 4, 4, SampleKind::F32 }) + b;
    if (rate_ != fileRate) line += ", resampled from " + std::to_string(fileRate) + " Hz";
    if (opt_.driftPpm != 0.0) { std::snprintf(b, sizeof(b), ", drift %+.1f ppm", opt_.driftPpm); line += b; }
    if (opt_.loop) line += ", loop";
    line += virtual_ ? ", virtual clock" : ", real clock";
    gate_->log(line);
    reporter_.set("open", "");
    return true;
}

void WavSource::begin() {
    if (virtual_) return;                       // the mixer pumps it (and opens it at the first pump)
    std::shared_ptr<WavSource> self = shared_from_this();
    thread_.start([self]() { self->threadMain(); });
}

void WavSource::requestStop() {
    if (stopEv_) ::SetEvent(stopEv_);
}

bool WavSource::join(DWORD ms) { return thread_.join(ms); }

void WavSource::pump(double hubSec) {
    if (!tried_) {
        tried_ = true;
        open();
        ::SetEvent(firstEv_);
    }
    produceUntil(hubSec);
}

void WavSource::threadMain() {
    tried_ = true;
    const bool ok = open();
    ::SetEvent(firstEv_);
    if (!ok) return;
    LARGE_INTEGER f = {}, t0 = {}, now = {};
    ::QueryPerformanceFrequency(&f);
    ::QueryPerformanceCounter(&t0);
    // The origin of this source's clock, for the capture smoke's latency measure ([C2]): a sample at file frame j
    // is "captured" at origin + (j + 1) / rate.
    char b[96];
    std::snprintf(b, sizeof(b), ": clock origin qpc=%lld freq=%lld", (long long)t0.QuadPart, (long long)f.QuadPart);
    gate_->log("capture: " + key_ + b);
    HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    for (;;) {
        DWORD w;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -100000;                     // 10 ms, relative (the packets come from the QPC count)
            ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE hs[2] = { stopEv_, timer };
            w = ::WaitForMultipleObjects(2, hs, FALSE, 100);
        } else {
            w = ::WaitForSingleObject(stopEv_, 10);
        }
        if (w == WAIT_OBJECT_0) break;
        ::QueryPerformanceCounter(&now);
        produceUntil((double)(now.QuadPart - t0.QuadPart) / (double)f.QuadPart);
    }
    if (timer) ::CloseHandle(timer);
}

void WavSource::fillPacket() {
    const size_t ch = ch_;
    for (uint32_t i = 0; i < packet_; ++i) {
        if (content_ >= frames_) {
            if (opt_.loop) content_ = 0;
        }
        if (content_ < frames_) {
            std::memcpy(&pkt_[(size_t)i * ch], &pcm_[(size_t)(content_ * ch)], ch * sizeof(float));
            ++content_;
        } else {
            std::fill(pkt_.begin() + (ptrdiff_t)((size_t)i * ch), pkt_.begin() + (ptrdiff_t)((size_t)(i + 1) * ch), 0.0f);
        }
    }
}

// The "device" fails at t: closed, and RetryPolicy says when it is tried again -- the WASAPI source's own sequence
// (TrCaptureWasapi.cpp threadMain), its log lines included, on this source's clock.
void WavSource::fail(double t, bool gap) {
    down_ = true;
    const char* state = gap ? "absent" : "error";
    const std::string why = gap ? "Test gap: the source is unplugged for a while."
                                : "Test flap: the source opens, then fails at once.";
    const RetryPolicy::Failure f = retry_.failed((uint64_t)std::llround(t * 1000.0), std::string(state) + "|" + why, 0);
    retryAt_ = t + (double)f.waitMs / 1000.0;
    reporter_.set(state, why);
    if (f.log)
        gate_->log("capture: " + key_ + ": test " + (gap ? "gap (unplugged)" : "flap (fails at once)") + " at "
                   + fmtMs(t * 1000.0) + " ms (retrying in " + fmtMs((double)f.waitMs) + " ms)");
    else if (f.still)
        gate_->log("capture: " + key_ + ": still " + state + " after " + std::to_string(f.tries) + " tries");
}

void WavSource::produceUntil(double hubSec) {
    if (!ok_) return;
    std::lock_guard<std::mutex> lk(pumpMu_);
    const double devSec = hubSec * (1.0 + opt_.driftPpm * 1e-6);
    const uint64_t due = devSec <= 0 ? 0 : (uint64_t)std::floor(devSec * (double)rate_);
    while (emitted_ + packet_ <= due) {
        const double t = (double)emitted_ / (double)rate_;
        const uint64_t tMs = (uint64_t)std::llround(t * 1000.0);
        if (!down_) {
            const bool gap = inSpans(opt_.gaps, t);
            const bool flap = !gap && inSpans(opt_.flaps, t) && t - openedAt_ >= 0.05 - 1e-9;
            if (gap || flap) {
                fail(t, gap);
            } else {
                bool reportOpen = false;
                if (retry_.healthy(tMs, reportOpen)) {
                    if (reportOpen) reporter_.set("open", "");
                    gate_->log("capture: " + key_ + ": steady again at " + fmtMs(t * 1000.0) + " ms");
                }
            }
        }
        if (down_ && t + 1e-9 >= retryAt_) {
            if (inSpans(opt_.gaps, t)) {
                fail(t, true);                       // still unplugged: the try fails at its open
            } else {
                down_ = false;
                openedAt_ = t;
                stalled_ = false;
                lastPushDev_ = t;
                if (retry_.opened(tMs)) {
                    gate_->log("capture: " + key_ + ": reopened at " + fmtMs(t * 1000.0) + " ms");
                    reporter_.set("open", "");
                }
            }
        }
        const bool silentSpan = down_ || inSpans(opt_.stalls, t);
        if (silentSpan) {
            // Nothing is delivered: the audio of this span is lost, as with a real unplug or a stalled driver.
            for (uint32_t i = 0; i < packet_; ++i) {
                if (content_ >= frames_ && opt_.loop) content_ = 0;
                if (content_ < frames_) ++content_;
            }
        } else {
            fillPacket();
            feed_->push(pkt_.data(), packet_, ch_, rate_);
            lastPushDev_ = (double)(emitted_ + packet_) / (double)rate_;
            if (stalled_) {
                stalled_ = false;
                gate_->log("capture: " + key_ + ": delivering again at " + fmtMs(t * 1000.0) + " ms");
                reporter_.set("open", "");
            }
        }
        emitted_ += packet_;
    }
    if (!down_ && !stalled_ && devSec - lastPushDev_ >= kStallSec) {
        stalled_ = true;
        gate_->log("capture: " + key_ + ": nothing delivered for 2 s (stalled)");
        reporter_.set("stalled", "No sound has arrived from this input for a while.");
    }
}

} // namespace cap
} // namespace pbt
