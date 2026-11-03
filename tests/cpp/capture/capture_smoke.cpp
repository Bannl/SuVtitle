// =============================================================================
// SuVtitle tests/cpp/capture/capture_smoke.cpp -- wave L1b (2026-10-25): the LIVE capture hub through its public
// contract (engine/TrCaptureApi.h) and its building blocks (engine/TrCaptureCore.h), against a fake ILiveSink /
// ILiveEvents. Built from the hub's sources + TrDecimator.cpp directly (build_capture_smoke.cmd): it does not
// depend on the engine's main_translator.cpp.
//   [C1]     the Decimator at 16 / 22.05 / 32 / 44.1 / 48 / 88.2 / 96 / 192 kHz: 1 kHz to 0.01 %, 9 kHz gone; the
//            hub's delay formula against the real Decimator
//   [ARITH]  channel pick / sum / trim exact; a channel the source lacks = silence; the NaN/inf/+-4 guard; three
//            strips on one source = ONE open
//   [FIFO]   drop-oldest and its count; a concurrent producer/consumer stress
//   [STATE]  INPUT only on a change; the gate closed = silence; a Defer scope delivers after its end
//   [REFUSE] what start() refuses; a test source below 16 kHz refused (INPUT error)
//   [LEVELS] ~5 Hz, the peak in dBFS, silence -120
//   [SET]    a replacement keeps the unchanged lanes (no re-open, same slot, audio continuous), a trim in place
//   [STOP]   stop() joins: no ILiveSink / ILiveEvents call once it returned (cycles, both clocks)
//   [C2]     bench_en on A, bench_zh on B in alternating turns (SUVTITLE_TEST_MEDIA): the per-lane energy names the
//            talker on 100 % of the turns and VAD segments; the front-end latency measured with clicks on QPC
//   [C3]     +-500 ppm for 10 min (virtual clock): slips ~ the drift, nothing dropped, every click within +-1 VAD
//            hop (30 ms) of the no-drift run
//   [C4]     @gap=12:3: zeros, INPUT absent -> open at the first try of the reopen ladder after the gap (15.2 s);
//            two gaps: the ladder starts again after a healthy stream (fix round RVA-8)
//   [C5]     @stall=2:30: zeros at a steady cadence (real clock), INPUT stalled -> open
//   [RESYNC] a source 2 % fast: resynced, never overflowing
//   [OVF]    a sink stalled 2.5 s (real clock): the FIFO drops the oldest and counts, the lane returns to its target
//   [REAL]   this PC: the source list (logged); the default output's loopback for 3 s; one application's loopback on
//            capture_tone.exe (started by this test, -60 dBFS, 1.5 s). NEVER an input (in:) is opened.
//   [TREE]   (fix round RVA-2 / RVA-4) the application root rule: on tables made by hand (a browser's child, a game a
//            launcher started, new Teams' WebView2, a meeting app a browser started, unreadable and recycled parents)
//            and on processes this test starts (cmd -> cmd -> ping: a same-name child climbs, a different-name one
//            does not). Real processes: Windows' own cmd.exe and PING.EXE, in a kill-on-close job, no window.
//   [BACKOFF] (fix round RVA-8) @flap: the reopens follow the ladder 0.2 / 1 / 2 / 5 s without resetting, INPUT does not
//            flap (open > error > open > error > open once steady), a handful of log lines for the whole episode
//   [LIST]   (fix round RVA-9) a request is answered across a concurrent stop(); the watching outlives the run; nothing
//            reaches ILiveEvents once the hub is destroyed (a real enumeration: nothing is opened)
//   --quick  everything on the virtual clock, no real device, no real-clock run (the mutation sentinels run this).
//   --only <TAG>  one section.
// Its media are generated in .\capture_media\ (the build script runs it in tests\cpp\work\capture). ASCII only.
// =============================================================================
#include "TrCaptureApi.h"
#include "TrCaptureCore.h"
#include "TrCaptureEnum.h"
#include "TrCaptureWav.h"

#include <tlhelp32.h>          // [TREE]: the children of the processes it starts

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "version.lib")

using namespace pbt;

namespace {

int g_pass = 0, g_fail = 0, g_nc = 0;
bool g_quick = false;
std::string g_only;
std::string g_media;                         // absolute, ends with a backslash

void ok(const std::string& name, bool cond, const std::string& extra = std::string()) {
    if (cond) ++g_pass; else ++g_fail;
    std::printf("  %s  %s%s%s\n", cond ? "PASS" : "FAIL", name.c_str(), extra.empty() ? "" : "  -> ", extra.c_str());
    std::fflush(stdout);
}
void info(const std::string& s) { std::printf("  info  %s\n", s.c_str()); std::fflush(stdout); }
void notChecked(const std::string& s) { ++g_nc; std::printf("  NOT CHECKED  %s\n", s.c_str()); std::fflush(stdout); }
bool section(const char* tag) {
    if (!g_only.empty() && g_only != tag) return false;
    std::printf("[%s]\n", tag);
    return true;
}
std::string F(const char* fmt, ...) {
    char b[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return b;
}
int64_t qpc() { LARGE_INTEGER t; ::QueryPerformanceCounter(&t); return t.QuadPart; }
double qpcFreq() { LARGE_INTEGER f; ::QueryPerformanceFrequency(&f); return (double)f.QuadPart; }
bool waitFor(const std::function<bool()>& f, DWORD ms) {
    const ULONGLONG t0 = ::GetTickCount64();
    while (::GetTickCount64() - t0 < ms) { if (f()) return true; ::Sleep(2); }
    return f();
}
constexpr double kPi = 3.14159265358979323846;
// The trim law (amplitude dB), written HERE and not taken from the product: a test that asks the product's own
// conversion for the expected value proves nothing (capmut's "10*log instead of 20*log" mutant survived until then).
float dbToGain(double db) { return (float)std::pow(10.0, db / 20.0); }

// ---- media -----------------------------------------------------------------------------------------------------
bool writeWav(const std::string& path, uint32_t rate, uint32_t ch, const std::vector<float>& x, bool asFloat) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    const uint32_t bps = asFloat ? 4u : 2u;
    const uint32_t frames = (uint32_t)(x.size() / ch);
    const uint32_t data = frames * ch * bps;
    const uint32_t fmtLen = asFloat ? 18u : 16u;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f); w32(4 + 8 + fmtLen + 8 + data); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); w32(fmtLen);
    w16(asFloat ? 3 : 1); w16((uint16_t)ch); w32(rate); w32(rate * ch * bps); w16((uint16_t)(ch * bps)); w16((uint16_t)(bps * 8));
    if (asFloat) w16(0);
    std::fwrite("data", 1, 4, f); w32(data);
    if (asFloat) {
        std::fwrite(x.data(), 4, (size_t)frames * ch, f);
    } else {
        for (size_t i = 0; i < (size_t)frames * ch; ++i) {
            const float v = std::max(-1.0f, std::min(1.0f, x[i]));
            w16((uint16_t)(int16_t)std::lround(v * 32767.0f));
        }
    }
    std::fclose(f);
    return true;
}

// The bench recordings: 16-bit PCM. Read here independently of the product's parser.
bool readWav16Mono(const std::string& path, uint32_t& rate, std::vector<float>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<unsigned char> d;
    unsigned char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 44 || std::memcmp(d.data(), "RIFF", 4) || std::memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t at = 12;
    uint16_t ch = 0, bits = 0;
    while (at + 8 <= d.size()) {
        uint32_t len;
        std::memcpy(&len, d.data() + at + 4, 4);
        const unsigned char* body = d.data() + at + 8;
        if (!std::memcmp(d.data() + at, "fmt ", 4)) {
            std::memcpy(&ch, body + 2, 2);
            std::memcpy(&rate, body + 4, 4);
            std::memcpy(&bits, body + 14, 2);
        } else if (!std::memcmp(d.data() + at, "data", 4) && bits == 16 && ch >= 1) {
            const size_t frames = std::min<size_t>(len, d.size() - (at + 8)) / (2u * ch);
            out.resize(frames);
            for (size_t i = 0; i < frames; ++i) {
                int16_t s;
                std::memcpy(&s, body + i * 2u * ch, 2);
                out[i] = (float)s / 32768.0f;
            }
            return true;
        }
        at += 8 + (size_t)len + (len & 1u);
    }
    return false;
}

std::vector<float> sine(uint32_t rate, double sec, double hz, float amp) {
    std::vector<float> x((size_t)(rate * sec));
    for (size_t i = 0; i < x.size(); ++i) x[i] = amp * (float)std::sin(2.0 * kPi * hz * (double)i / rate);
    return x;
}
std::vector<float> clicks(uint32_t rate, double sec, double first, double every, float amp) {
    std::vector<float> x((size_t)(rate * sec), 0.0f);
    for (double t = first; t < sec - 1e-9; t += every) {
        const size_t i = (size_t)std::llround(t * rate);
        if (i < x.size()) x[i] = amp;
    }
    return x;
}
std::string media(const char* name) { return g_media + name; }

// ---- the fakes ---------------------------------------------------------------------------------------------------
struct FakeSink : ILiveSink {
    std::mutex mu;
    bool   record = true;
    size_t cap = 0;                                     // max frames recorded (0 = all)
    bool   timing = false;                              // keep QPC + frame index of every call
    std::vector<float> mix, lane[kMaxLiveInputs];
    std::vector<std::array<std::string, kMaxLiveInputs>> binds;
    std::vector<uint64_t> bindAt;
    std::vector<int64_t> callQpc;
    std::vector<uint64_t> callAt;
    std::atomic<uint64_t> frames{ 0 };
    std::atomic<bool> stopped{ false };
    std::atomic<int> afterStop{ 0 };
    DWORD tid = 0;
    bool twoThreads = false, badN = false, badSample = false, badMix = false, laneWithoutBind = false;
    std::function<void(uint64_t)> hook;                 // on the mixer thread, before recording

    void bindInputs(const std::string (&keys)[kMaxLiveInputs]) override {
        if (stopped.load()) ++afterStop;
        std::lock_guard<std::mutex> lk(mu);
        std::array<std::string, kMaxLiveInputs> a;
        for (uint32_t k = 0; k < kMaxLiveInputs; ++k) a[k] = keys[k];
        binds.push_back(a);
        bindAt.push_back(frames.load());
    }
    void feed16k(const float* m, int n, const float* const (&per)[kMaxLiveInputs]) override {
        if (stopped.load()) ++afterStop;
        if (hook) hook(frames.load());
        std::lock_guard<std::mutex> lk(mu);
        const DWORD me = ::GetCurrentThreadId();
        if (!tid) tid = me; else if (tid != me) twoThreads = true;
        if (n < 1 || n > kFeedMaxFrames) { badN = true; return; }
        for (int i = 0; i < n; ++i) {
            float s = 0.0f;
            bool clamped = false;
            for (uint32_t k = 0; k < kMaxLiveInputs; ++k) {
                if (!per[k]) continue;
                const float v = per[k][i];
                if (!(v == v) || v > 4.0f || v < -4.0f) badSample = true;
                s += v;
            }
            if (!(m[i] == m[i]) || m[i] > 4.0f || m[i] < -4.0f) badSample = true;
            if (s > 4.0f || s < -4.0f) clamped = true;
            if (!clamped && std::fabs(s - m[i]) > 1e-5f) badMix = true;
        }
        for (uint32_t k = 0; k < kMaxLiveInputs; ++k)
            if (per[k] && (binds.empty() || binds.back()[k].empty())) laneWithoutBind = true;
        if (timing) { callQpc.push_back(qpc()); callAt.push_back(frames.load()); }
        if (record && (!cap || mix.size() < cap)) {
            mix.insert(mix.end(), m, m + n);
            for (uint32_t k = 0; k < kMaxLiveInputs; ++k) {
                if (per[k]) lane[k].insert(lane[k].end(), per[k], per[k] + n);
                else        lane[k].insert(lane[k].end(), (size_t)n, 0.0f);
            }
        }
        frames.fetch_add((uint64_t)n);
    }
    bool sane() const { return !twoThreads && !badN && !badSample && !badMix && !laneWithoutBind; }
    std::string why() const {
        return F("twoThreads=%d badN=%d badSample=%d badMix=%d laneWithoutBind=%d", twoThreads, badN, badSample, badMix,
                 laneWithoutBind);
    }
};

struct FakeEvents : ILiveEvents {
    struct State { std::string key, state, why; uint64_t frame; };
    std::mutex mu;
    std::vector<State> states;
    std::vector<std::vector<std::pair<std::string, float>>> levels;
    std::vector<uint64_t> levelAt;
    std::vector<std::vector<SourceInfo>> lists;
    std::vector<std::string> logs;
    std::atomic<bool> stopped{ false };
    std::atomic<int> afterStop{ 0 };
    // fix round RVA-9: after stop() the RUN's events (INPUT, LEVELS) must stop; the list watching (onSources, its log
    // lines) may go on until the hub is DESTROYED -- after which nothing at all.
    std::atomic<int> runAfterStop{ 0 };
    std::atomic<bool> destroyed{ false };
    std::atomic<int> afterDestroy{ 0 };
    FakeSink* sink = nullptr;
    bool echo = false;

    uint64_t now() const { return sink ? sink->frames.load() : 0; }
    void onInputState(const std::string& key, const std::string& state, const std::string& why) override {
        if (stopped.load()) { ++afterStop; ++runAfterStop; }
        if (destroyed.load()) ++afterDestroy;
        std::lock_guard<std::mutex> lk(mu);
        states.push_back({ key, state, why, now() });
        if (echo) info("INPUT " + state + " " + key.substr(0, 60) + (why.empty() ? "" : " -- " + why));
    }
    void onLevels(const std::vector<std::pair<std::string, float>>& peaks) override {
        if (stopped.load()) { ++afterStop; ++runAfterStop; }
        if (destroyed.load()) ++afterDestroy;
        std::lock_guard<std::mutex> lk(mu);
        levels.push_back(peaks);
        levelAt.push_back(now());
    }
    void onSources(const std::vector<SourceInfo>& list) override {
        if (stopped.load()) ++afterStop;
        if (destroyed.load()) ++afterDestroy;
        std::lock_guard<std::mutex> lk(mu);
        lists.push_back(list);
    }
    void onLog(const std::string& line) override {
        if (stopped.load()) ++afterStop;
        if (destroyed.load()) ++afterDestroy;
        std::lock_guard<std::mutex> lk(mu);
        logs.push_back(line);
        if (echo) info(line);
    }
    int countLog(const std::string& prefix) {
        std::lock_guard<std::mutex> lk(mu);
        int n = 0;
        for (const auto& l : logs) if (l.compare(0, prefix.size(), prefix) == 0) ++n;
        return n;
    }
    bool hasLog(const std::string& part) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& l : logs) if (l.find(part) != std::string::npos) return true;
        return false;
    }
    std::string logWith(const std::string& part) {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& l : logs) if (l.find(part) != std::string::npos) return l;
        return std::string();
    }
    std::vector<State> statesOf(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<State> v;
        for (const auto& s : states) if (s.key == key) v.push_back(s);
        return v;
    }
    size_t listCount() { std::lock_guard<std::mutex> lk(mu); return lists.size(); }
    std::vector<SourceInfo> lastList() { std::lock_guard<std::mutex> lk(mu); return lists.empty() ? std::vector<SourceInfo>() : lists.back(); }
    void dump() {
        std::lock_guard<std::mutex> lk(mu);
        for (const auto& l : logs) info("log: " + l);
        for (const auto& s : states) info(F("state @%.3f s: ", (double)s.frame / kAsrRate) + s.state + " " + s.key.substr(0, 50) + " " + s.why);
    }
};

std::string stateSeq(const std::vector<FakeEvents::State>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : ">") + x.state;
    return s;
}

struct StatsLine {
    bool found = false;
    double fed = 0, fill = 0, target = 0, delay = 0;
    unsigned long long dropped = 0, underruns = 0, up = 0, down = 0, resyncs = 0;
};
StatsLine lastStats(FakeEvents& ev, const std::string& key, const std::string& chan) {
    StatsLine s;
    const std::string prefix = "capture: stats " + key + " " + chan + ": ";
    std::lock_guard<std::mutex> lk(ev.mu);
    for (auto it = ev.logs.rbegin(); it != ev.logs.rend(); ++it) {
        if (it->compare(0, prefix.size(), prefix) != 0) continue;
        const char* p = it->c_str() + prefix.size();
        double period = 0, dec = 0;
        const int n = std::sscanf(p, "fed %lf s, dropped %llu, underruns %llu, slips +%llu/-%llu, resyncs %llu, fill %lf ms, "
                                     "target %lf ms, period %lf ms, decimator %lf ms, delay %lf ms",
                                  &s.fed, &s.dropped, &s.underruns, &s.up, &s.down, &s.resyncs, &s.fill, &s.target,
                                  &period, &dec, &s.delay);
        s.found = n == 11;
        break;
    }
    return s;
}
std::string statsText(const StatsLine& s) {
    return F("fed %.1f s, dropped %llu, underruns %llu, slips +%llu/-%llu, resyncs %llu, fill %.1f ms, target %.1f ms",
             s.fed, s.dropped, s.underruns, s.up, s.down, s.resyncs, s.fill, s.target);
}

LiveInput strip(const std::string& key, uint32_t chan = 0, double gainDb = 0.0) {
    LiveInput l;
    l.key = key;
    l.label = "strip";
    l.chan = chan;
    l.gainDb = gainDb;
    return l;
}

// A hub with its fakes, torn down in the right order (the hub first: it must not outlive what it calls).
struct Rig {
    FakeEvents ev;
    FakeSink   sink;
    std::unique_ptr<TrCaptureHub> hub;
    explicit Rig(bool virtualClock, bool testSources = true, uint32_t appPid = 0) {
        ev.sink = &sink;
        HubOptions o;
        o.testSources = testSources;
        o.virtualClock = virtualClock;
        o.appPid = appPid;
        hub.reset(new TrCaptureHub(&ev, o));
    }
    ~Rig() { finish(); }
    void finish() {
        if (!hub) return;
        hub->stop();
        sink.stopped = true;
        ev.stopped = true;
        hub.reset();
    }
    bool frames(double sec, DWORD timeoutMs) {
        const uint64_t want = (uint64_t)(sec * kAsrRate);
        return waitFor([&] { return sink.frames.load() >= want; }, timeoutMs);
    }
};

float meanOf(const std::vector<float>& x, double a, double b) {
    const size_t i0 = (size_t)(a * kAsrRate), i1 = std::min(x.size(), (size_t)(b * kAsrRate));
    double s = 0;
    for (size_t i = i0; i < i1; ++i) s += x[i];
    return i1 > i0 ? (float)(s / (double)(i1 - i0)) : 0.0f;
}
float maxDev(const std::vector<float>& x, double a, double b, float v) {
    const size_t i0 = (size_t)(a * kAsrRate), i1 = std::min(x.size(), (size_t)(b * kAsrRate));
    float d = 0;
    for (size_t i = i0; i < i1; ++i) d = std::max(d, std::fabs(x[i] - v));
    return i1 > i0 ? d : 1e9f;
}
double rmsOf(const std::vector<float>& x, size_t a, size_t b) {
    b = std::min(b, x.size());
    double s = 0;
    for (size_t i = a; i < b; ++i) s += (double)x[i] * x[i];
    return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}
bool allZero(const std::vector<float>& x, double a, double b) {
    const size_t i0 = (size_t)(a * kAsrRate), i1 = std::min(x.size(), (size_t)(b * kAsrRate));
    if (i1 <= i0) return false;
    for (size_t i = i0; i < i1; ++i) if (x[i] != 0.0f) return false;
    return true;
}
std::vector<size_t> findPeaks(const std::vector<float>& x, float thr, size_t spacing) {
    std::vector<size_t> out;
    size_t i = 0;
    while (i < x.size()) {
        if (std::fabs(x[i]) >= thr) {
            size_t best = i;
            for (size_t j = i; j < std::min(x.size(), i + 64); ++j) if (std::fabs(x[j]) > std::fabs(x[best])) best = j;
            out.push_back(best);
            i = best + spacing;
        } else {
            ++i;
        }
    }
    return out;
}
double goertzel(const std::vector<float>& x, size_t a, size_t b, double hz) {
    b = std::min(b, x.size());
    const double w = 2.0 * kPi * hz / kAsrRate, c = 2.0 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t i = a; i < b; ++i) { const double s0 = x[i] + c * s1 - s2; s2 = s1; s1 = s0; }
    return s1 * s1 + s2 * s2 - c * s1 * s2;
}

// =================================================================================================================
// [C1] the Decimator
// =================================================================================================================
void testC1() {
    const uint32_t rates[] = { 16000, 22050, 32000, 44100, 48000, 88200, 96000, 192000 };
    for (uint32_t r : rates) {
        // 1 kHz at 0.5, fed in 10 ms packets; amplitude fitted on 1 s of the output (1000 whole cycles at 16 kHz)
        {
            Decimator d;
            d.init(r);
            const std::vector<float> x = sine(r, 2.0, 1000.0, 0.5f);
            std::vector<float> y;
            const size_t pk = r / 100;
            for (size_t i = 0; i < x.size(); i += pk) d.process(&x[i], (int)std::min(pk, x.size() - i), y);
            const size_t s0 = 8000, n = 16000;
            double a = 0, b = 0;
            if (y.size() >= s0 + n)
                for (size_t k = 0; k < n; ++k) {
                    const double t = 2.0 * kPi * 1000.0 * (double)(s0 + k) / kAsrRate;
                    a += y[s0 + k] * std::sin(t);
                    b += y[s0 + k] * std::cos(t);
                }
            const double amp = 2.0 * std::sqrt(a * a + b * b) / (double)n;
            ok(F("[C1] %u Hz: 1 kHz reproduced to 0.01 %%", r), std::fabs(amp / 0.5 - 1.0) <= 1e-4, F("gain %.6f", amp / 0.5));
        }
        // 9 kHz: gone (only where the input can carry it)
        if (r > 18000) {
            Decimator d;
            d.init(r);
            const std::vector<float> x = sine(r, 2.0, 9000.0, 0.5f);
            std::vector<float> y;
            const size_t pk = r / 100;
            for (size_t i = 0; i < x.size(); i += pk) d.process(&x[i], (int)std::min(pk, x.size() - i), y);
            const double rms = rmsOf(y, 8000, 24000);
            const double db = 20.0 * std::log10(std::max(rms, 1e-12) / (0.5 / std::sqrt(2.0)));
            ok(F("[C1] %u Hz: a 9 kHz tone is gone (< -60 dB)", r), db < -60.0, F("%.1f dB", db));
        }
        // the hub's delay formula (inputDelayMs) against the real Decimator: an impulse at K, fed one sample at a
        // time; its output sample leaves when the input has gone `ht` samples past it
        {
            Decimator d;
            d.init(r);
            const size_t K = r / 10;                     // 100 ms: the output index is exactly 1600
            std::vector<float> y;
            std::vector<size_t> emitAt;
            for (size_t i = 0; i < K + 4000; ++i) {
                const float v = i == K ? 1.0f : 0.0f;
                const size_t before = y.size();
                d.process(&v, 1, y);
                for (size_t j = before; j < y.size(); ++j) emitAt.push_back(i);
            }
            const size_t j0 = 1600;
            size_t peak = 0;
            for (size_t j = 0; j < y.size(); ++j) if (std::fabs(y[j]) > std::fabs(y[peak])) peak = j;
            const double measured = j0 < emitAt.size() ? (double)(emitAt[j0] - K) : -1.0;
            const double formula = cap::decimatorDelayMs(r) * r / 1000.0;
            ok(F("[C1] %u Hz: inputDelayMs's decimator term == the Decimator's own latency", r),
               peak == j0 && std::fabs(measured - formula) <= 1.0,
               F("peak at %zu (want %zu), latency %.0f samples, formula %.1f (%.2f ms)", peak, j0, measured, formula,
                 cap::decimatorDelayMs(r)));
        }
    }
}

// =================================================================================================================
// [ARITH] pick / sum / trim / guard / one open
// =================================================================================================================
void testArith() {
    // 4 channels of constants, float32 48 kHz, 2 s
    const float c[4] = { 0.1f, 0.02f, -0.05f, 0.3f };
    std::vector<float> x((size_t)96000 * 4);
    for (size_t f = 0; f < 96000; ++f) for (int k = 0; k < 4; ++k) x[f * 4 + (size_t)k] = c[k];
    const std::string w4 = media("arith4.wav");
    writeWav(w4, 48000, 4, x, true);
    const std::string key = "wav:" + w4;
    {
        Rig rig(true);
        rig.hub->start({ strip(key, 0, 6.0) }, &rig.sink);
        rig.frames(1.6, 20000);
        rig.finish();
        const float want = (c[0] + c[1] + c[2] + c[3]) * dbToGain(6.0);
        ok("[ARITH] the MIX strip = the SUM of the channels x trim (+6 dB)", maxDev(rig.sink.lane[0], 0.5, 1.5, want) < 1e-5f,
           F("%.6f, want %.6f", meanOf(rig.sink.lane[0], 0.5, 1.5), want));
        ok("[ARITH] the sink is fed sane blocks (one thread, n <= 160, finite, +-4, mix = sum)", rig.sink.sane(), rig.sink.why());
    }
    {
        Rig rig(true);
        rig.hub->start({ strip(key, 1, -6.0), strip(key, 3, 0.0), strip(key, 4, 3.0) }, &rig.sink);
        rig.frames(1.6, 20000);
        rig.finish();
        const float w0 = c[0] * dbToGain(-6.0), w1 = c[2], w2 = c[3] * dbToGain(3.0);
        const bool exact = maxDev(rig.sink.lane[0], 0.5, 1.5, w0) < 1e-5f && maxDev(rig.sink.lane[1], 0.5, 1.5, w1) < 1e-5f &&
                           maxDev(rig.sink.lane[2], 0.5, 1.5, w2) < 1e-5f;
        ok("[ARITH] three channel strips: each its own channel x its own trim", exact,
           F("%.6f %.6f %.6f, want %.6f %.6f %.6f", meanOf(rig.sink.lane[0], 0.5, 1.5), meanOf(rig.sink.lane[1], 0.5, 1.5),
             meanOf(rig.sink.lane[2], 0.5, 1.5), w0, w1, w2));
        ok("[ARITH] the mix = their sum", maxDev(rig.sink.mix, 0.5, 1.5, w0 + w1 + w2) < 2e-5f,
           F("%.6f, want %.6f", meanOf(rig.sink.mix, 0.5, 1.5), w0 + w1 + w2));
        ok("[ARITH] three strips on ONE source = ONE open", rig.ev.countLog("capture: opened " + key) == 1,
           F("%d opens", rig.ev.countLog("capture: opened " + key)));
        // TrPipe.h LANES (R1): a one-channel strip is named "<key>|<chan>" -- two channels of one source never share a name
        ok("[ARITH] bound under their LANE ID, slot by slot (key|1, key|3, key|4)",
           !rig.sink.binds.empty() && rig.sink.binds[0][0] == key + "|1" && rig.sink.binds[0][1] == key + "|3" &&
           rig.sink.binds[0][2] == key + "|4" && rig.sink.binds[0][3].empty());
        ok("[ARITH] sane blocks", rig.sink.sane(), rig.sink.why());
    }
    {
        Rig rig(true);
        rig.hub->start({ strip(key, 6, 0.0) }, &rig.sink);
        rig.frames(1.0, 20000);
        rig.finish();
        ok("[ARITH] a channel the source does not have: the strip carries silence, said once in the log",
           allZero(rig.sink.lane[0], 0.0, 0.9) && rig.ev.countLog("capture: " + key + ": channel 6 does not exist") == 1);
    }
    {
        // the guard: NaN, +inf, -inf -> 0; +-10 and 0.5 x +24 dB -> clamped to +-4
        std::vector<float> g;
        auto seg = [&](float v, double s) { g.insert(g.end(), (size_t)(48000 * s), v); };
        seg(std::nanf(""), 0.3); seg(INFINITY, 0.3); seg(-INFINITY, 0.3); seg(10.0f, 0.5); seg(-10.0f, 0.5); seg(0.5f, 0.5);
        const std::string wg = media("guard.wav");
        writeWav(wg, 48000, 1, g, true);
        Rig rig(true);
        rig.hub->start({ strip("wav:" + wg, 0, 24.0) }, &rig.sink);
        rig.frames(2.6, 20000);
        rig.finish();
        const auto& l = rig.sink.lane[0];
        const bool zeros = maxDev(l, 0.12, 0.28, 0.0f) == 0.0f && maxDev(l, 0.42, 0.58, 0.0f) == 0.0f && maxDev(l, 0.72, 0.88, 0.0f) == 0.0f;
        const bool clamps = maxDev(l, 1.0, 1.35, 4.0f) < 1e-5f && maxDev(l, 1.5, 1.85, -4.0f) < 1e-5f && maxDev(l, 2.0, 2.35, 4.0f) < 1e-5f;
        ok("[ARITH] NaN / +inf / -inf become silence", zeros);
        ok("[ARITH] +-10 and 0.5 at +24 dB are clamped to +-4", clamps,
           F("%.4f %.4f %.4f", meanOf(l, 1.0, 1.35), meanOf(l, 1.5, 1.85), meanOf(l, 2.0, 2.35)));
        ok("[ARITH] every sample handed on is finite and within +-4", rig.sink.sane(), rig.sink.why());
    }
}

// =================================================================================================================
// [FIFO]
// =================================================================================================================
void testFifo() {
    {
        cap::LaneFifo f;
        std::vector<float> v(40000);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (float)i;
        f.push(v.data(), (int)v.size());
        std::vector<float> o(cap::kFifoCapacity);
        const int n = f.read(o.data(), (int)o.size());
        ok("[FIFO] 40000 pushed into 32768: the OLDEST 7232 dropped and counted",
           f.dropped() == 7232 && n == cap::kFifoCapacity && o[0] == 7232.0f && o[(size_t)n - 1] == 39999.0f,
           F("dropped %llu, read %d, first %.0f, last %.0f", (unsigned long long)f.dropped(), n, o[0], o[(size_t)n - 1]));
    }
    {
        cap::LaneFifo f;
        std::vector<float> v(cap::kFifoCapacity + 100);
        for (size_t i = 0; i < v.size(); ++i) v[i] = (float)i;
        f.push(v.data(), 100);
        float tmp[50];
        f.read(tmp, 50);                                      // 50..99 left
        f.push(v.data() + 100, cap::kFifoCapacity);           // 100 .. 32867: 50 too many
        float first = -1;
        f.read(&first, 1);
        ok("[FIFO] a full FIFO gives up exactly its oldest (50..99 dropped, 100 first)", f.dropped() == 50 && first == 100.0f,
           F("dropped %llu, first %.0f", (unsigned long long)f.dropped(), first));
    }
    {
        // A producer thread at full speed in bursts, a consumer that pauses now and then (the FIFO overflows):
        // everything read is strictly increasing, and read + dropped = pushed.
        cap::LaneFifo f;
        const uint32_t total = 6000000;
        std::atomic<bool> done{ false };
        std::thread prod([&] {
            std::mt19937 rng(1);
            std::vector<float> b(1000);
            uint32_t v = 0;
            while (v < total) {
                uint32_t n = 1 + rng() % 1000;
                if (v + n > total) n = total - v;
                for (uint32_t i = 0; i < n; ++i) b[i] = (float)(v + i);
                f.push(b.data(), (int)n);
                v += n;
            }
            done = true;
        });
        std::mt19937 rng(2);
        std::vector<float> b(1200);
        float last = -1.0f;
        uint64_t got = 0;
        bool order = true;
        while (!done.load() || f.size() > 0) {
            const int m = f.read(b.data(), 1 + (int)(rng() % 1200));
            for (int i = 0; i < m; ++i) { if (b[(size_t)i] <= last) order = false; last = b[(size_t)i]; }
            got += (uint64_t)m;
            if (rng() % 40 == 0) ::Sleep(2);
        }
        prod.join();
        ok("[FIFO] concurrent stress: strictly increasing, nothing duplicated", order);
        ok("[FIFO] concurrent stress: read + dropped == pushed, and the drop path ran", got + f.dropped() == total && f.dropped() > 0,
           F("read %llu + dropped %llu = %llu of %u", (unsigned long long)got, (unsigned long long)f.dropped(),
             (unsigned long long)(got + f.dropped()), total));
    }
}

// =================================================================================================================
// [STATE]
// =================================================================================================================
void testState() {
    FakeEvents ev;
    auto gate = std::make_shared<cap::EventGate>(&ev);
    cap::StateReporter r("k", gate);
    r.set("open", "");
    r.set("open", "again");
    r.set("absent", "a");
    r.set("absent", "b");
    r.set("open", "");
    ok("[STATE] INPUT only on a change: open, open, absent, absent, open -> open>absent>open", stateSeq(ev.statesOf("k")) == "open>absent>open",
       stateSeq(ev.statesOf("k")));
    {
        cap::EventGate::Defer d;
        gate->log("deferred");
        ok("[STATE] inside a Defer scope nothing is delivered yet", !ev.hasLog("deferred"));
    }
    ok("[STATE] ... and it is delivered when the scope ends", ev.hasLog("deferred"));
    gate->close();
    r.set("busy", "");
    gate->log("after close");
    ok("[STATE] a closed gate delivers nothing", stateSeq(ev.statesOf("k")) == "open>absent>open" && !ev.hasLog("after close"));
}

// =================================================================================================================
// [REFUSE]
// =================================================================================================================
void testRefuse() {
    const std::string w = media("sine16k.wav");
    writeWav(w, 16000, 1, sine(16000, 2.0, 440.0, 0.25f), false);
    const std::string key = "wav:" + w;
    {
        Rig rig(true, false);
        ok("[REFUSE] a wav: key on a product engine (no testSources)", !rig.hub->start({ strip(key) }, &rig.sink));
    }
    Rig rig(true);
    TrCaptureHub& h = *rig.hub;
    FakeSink& s = rig.sink;
    ok("[REFUSE] no sink", !h.start({ strip(key) }, nullptr));
    ok("[REFUSE] no input", !h.start({}, &s));
    ok("[REFUSE] five inputs", !h.start({ strip(key, 1), strip(key, 2), strip(key, 3), strip(key, 4), strip(key, 5) }, &s));
    ok("[REFUSE] ain: (reserved)", !h.start({ strip("ain:x") }, &s));
    ok("[REFUSE] an unknown kind", !h.start({ strip("sys:") }, &s) && !h.start({ strip("foo:x") }, &s));
    ok("[REFUSE] channel 9", !h.start({ strip(key, 9) }, &s));
    ok("[REFUSE] a trim of +30 dB, of NaN", !h.start({ strip(key, 0, 30.0) }, &s) && !h.start({ strip(key, 0, std::nan("")) }, &s));
    ok("[REFUSE] the same key twice on one channel, or next to its own mix",
       !h.start({ strip(key, 1), strip(key, 1) }, &s) && !h.start({ strip(key, 0), strip(key, 2) }, &s));
    ok("[REFUSE] a key holding '|', the lane separator (fix round RVA-10)",
       !h.start({ strip(key + "|1") }, &s) && !h.start({ strip("in:dev|2") }, &s));
    ok("[REFUSE] nothing ran", !h.running() && s.frames.load() == 0);
    ok("[REFUSE] ... but two different channels >= 1 of one key are fine", h.start({ strip(key, 1), strip(key, 2) }, &s));
    h.stop();
    {
        Rig r2(true);
        const std::string k8 = key + "@rate=8000";
        r2.hub->start({ strip(k8) }, &r2.sink);
        r2.frames(0.5, 20000);
        r2.finish();
        const auto st = r2.ev.statesOf(k8);
        ok("[REFUSE] a test source below 16 kHz: INPUT error, never handed to the Decimator",
           st.size() == 1 && st[0].state == "error" && st[0].why.find("16 kHz") != std::string::npos && allZero(r2.sink.lane[0], 0.0, 0.45),
           st.empty() ? "no state" : st[0].state + " -- " + st[0].why);
        Rig r3(true);
        const std::string kb = key + "@rate=abc";
        r3.hub->start({ strip(kb) }, &r3.sink);
        r3.frames(0.2, 20000);
        r3.finish();
        const auto sb = r3.ev.statesOf(kb);
        ok("[REFUSE] a malformed test option: INPUT error", sb.size() == 1 && sb[0].state == "error");
    }
    {
        Rig r4(true);
        r4.hub->start({ strip("in:{0.0.1.00000000}.{nope}") }, &r4.sink);
        r4.frames(0.2, 20000);
        r4.finish();
        const auto sd = r4.ev.statesOf("in:{0.0.1.00000000}.{nope}");
        ok("[REFUSE] a device on the virtual clock: INPUT unsupported, nothing opened", sd.size() == 1 && sd[0].state == "unsupported");
    }
}

// =================================================================================================================
// [LEVELS]
// =================================================================================================================
void testLevels() {
    const std::string wc = media("const05.wav"), ws = media("silence.wav");
    writeWav(wc, 48000, 1, std::vector<float>(48000 * 4, 0.5f), true);
    writeWav(ws, 48000, 1, std::vector<float>(48000 * 4, 0.0f), true);
    Rig rig(true);
    rig.hub->start({ strip("wav:" + wc), strip("wav:" + ws) }, &rig.sink);
    rig.frames(3.0, 20000);
    rig.finish();
    std::lock_guard<std::mutex> lk(rig.ev.mu);
    const size_t n = rig.ev.levels.size();
    const uint64_t fr = rig.sink.frames.load();
    bool cadence = true, values = true;
    std::string bad;
    for (size_t i = 0; i < n; ++i) {
        if (rig.ev.levelAt[i] % (uint64_t)(kAsrRate / 5) != 0) cadence = false;
        const auto& v = rig.ev.levels[i];
        if (v.size() != 2) { values = false; continue; }
        if (rig.ev.levelAt[i] >= (uint64_t)(kAsrRate / 2) && rig.ev.levelAt[i] <= (uint64_t)(3.5 * kAsrRate)) {
            if (std::fabs(v[0].second - (-6.02f)) > 0.05f || v[1].second != -120.0f) { values = false; bad = F("%.2f %.2f", v[0].second, v[1].second); }
        }
    }
    ok("[LEVELS] one LEVELS per 200 ms of the mixer's clock", cadence && n + 1 >= fr / (uint64_t)(kAsrRate / 5) && n <= fr / (uint64_t)(kAsrRate / 5),
       F("%zu over %.2f s", n, (double)fr / kAsrRate));
    ok("[LEVELS] the peak in dBFS: 0.5 -> -6.0, silence -> -120", values, bad);
}

// =================================================================================================================
// [SET] replacement
// =================================================================================================================
void testSet() {
    const std::string wa = media("setA.wav"), wb = media("setB.wav");
    writeWav(wa, 48000, 1, sine(48000, 3.0, 440.0, 0.25f), true);
    writeWav(wb, 44100, 1, sine(44100, 3.0, 1000.0, 0.25f), true);
    const std::string ka = "wav:" + wa + "@loop", kb = "wav:" + wb + "@loop";
    Rig rig(true);
    rig.hub->start({ strip(ka) }, &rig.sink);
    rig.frames(1.0, 20000);
    const uint64_t f1 = rig.sink.frames.load();
    rig.hub->start({ strip(ka), strip(kb) }, &rig.sink);                // B added
    rig.frames(2.0, 20000);
    const uint64_t f2 = rig.sink.frames.load();
    rig.hub->start({ strip(kb), strip(ka, 0, 6.0) }, &rig.sink);        // A's trim +6 dB (order swapped on purpose)
    rig.frames(3.0, 20000);
    const uint64_t f3 = rig.sink.frames.load();
    rig.hub->start({ strip(kb) }, &rig.sink);                           // A removed
    rig.frames(4.0, 20000);
    rig.finish();
    std::vector<std::array<std::string, kMaxLiveInputs>> b;
    {
        std::lock_guard<std::mutex> lk(rig.sink.mu);
        b = rig.sink.binds;
    }
    const bool seq = b.size() == 3 && b[0][0] == ka && b[0][1].empty() && b[1][0] == ka && b[1][1] == kb &&
                     b[2][0].empty() && b[2][1] == kb;
    ok("[SET] bindInputs: [A] -> [A,B] -> (trim: not re-bound) -> [-,B]: a kept lane never changes slot", seq,
       F("%zu binds", b.size()));
    ok("[SET] A was opened ONCE through three replacements, closed once", rig.ev.countLog("capture: opened " + ka) == 1 &&
       rig.ev.countLog("capture: closed " + ka) == 1);
    ok("[SET] B opened once", rig.ev.countLog("capture: opened " + kb) == 1);
    // A's audio is continuous across the replacement that added B: no run of zeros (an underrun / a new FIFO)
    const auto& la = rig.sink.lane[0];
    size_t run = 0, worst = 0;
    for (size_t i = (size_t)(0.3 * kAsrRate); i < std::min<size_t>(la.size(), (size_t)f3); ++i) {
        run = la[i] == 0.0f ? run + 1 : 0;
        worst = std::max(worst, run);
    }
    ok("[SET] A's lane never ran dry across the replacements", worst < 4, F("longest zero run %zu samples", worst));
    const double before = rmsOf(la, (size_t)f1, (size_t)f2), after = rmsOf(la, (size_t)f2 + 1600, (size_t)f3);
    ok("[SET] A's trim (+6 dB) applied in place", std::fabs(after / before - (double)dbToGain(6.0)) < 0.02,
       F("rms ratio %.4f, want %.4f", after / before, (double)dbToGain(6.0)));
    ok("[SET] sane blocks", rig.sink.sane(), rig.sink.why());
}

// =================================================================================================================
// [STOP]
// =================================================================================================================
void testStop() {
    const std::string wa = media("stopA.wav");
    writeWav(wa, 48000, 2, sine(48000, 2.0, 300.0, 0.2f), true);
    const std::string ka = "wav:" + wa + "@loop";
    int after = 0, listAfterStop = 0, afterGone = 0;
    ULONGLONG worst = 0;
    const int cycles = g_quick ? 8 : 20;
    bool restarted = true;
    for (int i = 0; i < cycles; ++i) {
        const bool virt = g_quick || i % 2 == 0;
        FakeEvents ev;
        FakeSink sink;
        ev.sink = &sink;
        HubOptions o;
        o.testSources = true;
        o.virtualClock = virt;
        {
            TrCaptureHub hub(&ev, o);
            hub.start({ strip(ka, 1), strip(ka, 2, -3.0) }, &sink);
            if (virt) waitFor([&] { return sink.frames.load() >= 1600; }, 5000);
            else ::Sleep(25 + (DWORD)i);
            if (i % 4 == 1) hub.requestSources();
            if (i == 2) {
                // stop, then start again on the same hub: it runs again
                hub.stop();
                const uint64_t f0 = sink.frames.load();
                hub.start({ strip(ka) }, &sink);
                restarted = waitFor([&] { return sink.frames.load() > f0 + 1600; }, 5000) && restarted;
            }
            const ULONGLONG t0 = ::GetTickCount64();
            hub.stop();
            worst = std::max(worst, ::GetTickCount64() - t0);
            sink.stopped = true;
            ev.stopped = true;
            ::Sleep(40);
            hub.stop();
        }
        ev.destroyed = true;
        ::Sleep(20);
        after += sink.afterStop.load() + ev.runAfterStop.load();
        listAfterStop += ev.afterStop.load() - ev.runAfterStop.load();
        afterGone += ev.afterDestroy.load();
    }
    ok(F("[STOP] no ILiveSink call and no INPUT / LEVELS once stop() returned (%d cycles)", cycles), after == 0,
       F("%d late calls", after));
    ok("[STOP] nothing at all once the hub is destroyed (the list watching ends with it: fix round RVA-9)", afterGone == 0,
       F("%d calls after the destructor; %d list / log calls between stop() and it (allowed)", afterGone, listAfterStop));
    ok("[STOP] stop() returns promptly", worst < 1500, F("worst %llu ms", (unsigned long long)worst));
    ok("[STOP] a stopped hub starts again", restarted);
}

// =================================================================================================================
// [C2] two talkers, two lanes
// =================================================================================================================
void testC2() {
    const char* tm = std::getenv("SUVTITLE_TEST_MEDIA");
    uint32_t ra = 0, rb = 0;
    std::vector<float> en, zh;
    if (!tm || !readWav16Mono(std::string(tm) + "\\bench_en.wav", ra, en) || !readWav16Mono(std::string(tm) + "\\bench_zh.wav", rb, zh) ||
        ra != 16000 || rb != 16000) {
        notChecked("[C2] attribution: needs SUVTITLE_TEST_MEDIA with bench_en.wav / bench_zh.wav (16 kHz)");
        return;
    }
    // Alternating turns of 2.5 s (A = English, B = Chinese), 0.5 s apart, after a 0.5 s lead-in: each file holds its
    // talker's turns and digital silence elsewhere.
    const double turn = 2.5, gap = 0.5, lead = 0.5;
    const int turns = 8;
    const double total = lead + turns * (turn + gap) + 0.5;
    std::vector<float> A((size_t)(total * 16000), 0.0f), B(A.size(), 0.0f);
    for (int t = 0; t < turns; ++t) {
        const std::vector<float>& src = (t % 2 == 0) ? en : zh;
        std::vector<float>& dst = (t % 2 == 0) ? A : B;
        const size_t from = (size_t)((t / 2) * turn * 16000), at = (size_t)((lead + t * (turn + gap)) * 16000);
        for (size_t i = 0; i < (size_t)(turn * 16000) && from + i < src.size(); ++i) dst[at + i] = src[from + i];
    }
    writeWav(media("turnsA.wav"), 16000, 1, A, false);
    writeWav(media("turnsB.wav"), 16000, 1, B, false);
    // put at 48 kHz by the source (@rate), so the hub decimates a real ratio
    const std::string ka = "wav:" + media("turnsA.wav") + "@rate=48000", kb = "wav:" + media("turnsB.wav") + "@rate=48000";
    Rig rig(g_quick);
    rig.hub->start({ strip(ka), strip(kb) }, &rig.sink);
    rig.frames(total, (DWORD)(total * 1000) + 20000);
    rig.finish();
    const auto& la = rig.sink.lane[0];
    const auto& lb = rig.sink.lane[1];
    const auto& mx = rig.sink.mix;
    ok("[C2] lane 0 is bound to A's key, lane 1 to B's", !rig.sink.binds.empty() && rig.sink.binds[0][0] == ka && rig.sink.binds[0][1] == kb);
    // turn by turn: the energy of each lane over the turn (shifted by a generous front-end delay, trimmed at both ends)
    const double lat = 0.06;
    int right = 0;
    for (int t = 0; t < turns; ++t) {
        const double a = lead + t * (turn + gap) + lat + 0.1, b = a + turn - 0.2;
        const size_t i0 = (size_t)(a * kAsrRate), i1 = (size_t)(b * kAsrRate);
        const double ea = rmsOf(la, i0, i1), eb = rmsOf(lb, i0, i1);
        if ((t % 2 == 0) == (ea > eb)) ++right;
    }
    ok("[C2] the per-lane energy names the talker of every turn", right == turns, F("%d / %d turns", right, turns));
    // segment by segment, as TrAudio does: a VAD on the mix (30 ms hops, open > -40 dBFS, close after 300 ms < -45)
    int segs = 0, segRight = 0, overlap = 0;
    {
        const size_t hop = 480;
        bool open = false;
        size_t start = 0, quiet = 0;
        double sa = 0, sb = 0;
        for (size_t h = 0; h + hop <= mx.size(); h += hop) {
            const double db = 20.0 * std::log10(std::max(rmsOf(mx, h, h + hop), 1e-9));
            if (!open && db > -40.0) { open = true; start = h; quiet = 0; sa = sb = 0; }
            if (!open) continue;
            for (size_t i = h; i < h + hop; ++i) { sa += (double)la[i] * la[i]; sb += (double)lb[i] * lb[i]; }
            quiet = db < -45.0 ? quiet + 1 : 0;
            if (quiet >= 10 || h + 2 * hop > mx.size()) {
                open = false;
                const double mid = ((double)(start + h) / 2.0) / kAsrRate - lat;
                const int t = (int)std::floor((mid - lead) / (turn + gap));
                const double inTurn = (mid - lead) - t * (turn + gap);
                if (t < 0 || t >= turns || inTurn > turn + 0.3) { ++overlap; continue; }
                ++segs;
                if ((t % 2 == 0) == (sa > sb)) ++segRight;
            }
        }
    }
    ok("[C2] VAD segments: the dominant lane is the talker on 100 % of them", segs > 0 && segRight == segs,
       F("%d / %d segments (%d outside the turns)", segRight, segs, overlap));
    ok("[C2] sane blocks", rig.sink.sane(), rig.sink.why());
}

void testC2Latency() {
    // a click every 0.5 s; the sink stamps every block with QPC; the source logs the QPC origin of its clock
    const uint32_t rate = 48000;
    const std::string w = media("latclicks.wav");
    writeWav(w, rate, 1, clicks(rate, 6.0, 0.5, 0.5, 0.5f), true);
    const std::string key = "wav:" + w;
    Rig rig(false);
    rig.sink.timing = true;
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(3.0, 10000);
    const double reported = rig.hub->inputDelayMs(key);
    rig.frames(6.3, 10000);
    rig.finish();
    const std::string l = rig.ev.logWith(key + ": clock origin qpc=");
    long long origin = 0, freq = 0;
    const size_t at = l.find("qpc=");
    if (at != std::string::npos) std::sscanf(l.c_str() + at, "qpc=%lld freq=%lld", &origin, &freq);
    const std::vector<size_t> pk = findPeaks(rig.sink.lane[0], 0.05f, 4000);
    std::vector<double> lat;
    for (size_t k = 0; k < pk.size(); ++k) {
        const size_t m = pk[k];
        const double srcSec = (double)m / kAsrRate;                // the click's place in the source's own timeline
        const double j = std::round(srcSec * 2.0) / 2.0 * rate;   // its frame in the file (a multiple of 0.5 s)
        size_t c = 0;
        while (c + 1 < rig.sink.callAt.size() && rig.sink.callAt[c + 1] <= m) ++c;
        if (c >= rig.sink.callQpc.size() || !freq) continue;
        const double tDev = (double)origin + (j + 1.0) * (double)freq / rate;
        lat.push_back(((double)rig.sink.callQpc[c] - tDev) * 1000.0 / (double)freq);
    }
    double mean = 0, mn = 1e9, mxv = -1e9;
    for (double v : lat) { mean += v; mn = std::min(mn, v); mxv = std::max(mxv, v); }
    if (!lat.empty()) mean /= (double)lat.size();
    ok("[C2] latency: all 11 clicks found", lat.size() == 11, F("%zu", lat.size()));
    ok("[C2] latency measured from the source's capture instant to the VAD (QPC)", !lat.empty() && mn > 5.0 && mxv < 200.0,
       F("mean %.1f ms, min %.1f, max %.1f", mean, mn, mxv));
    ok("[C2] inputDelayMs agrees with the measure (within 20 ms)", reported > 0 && std::fabs(reported - mean) <= 20.0,
       F("inputDelayMs %.1f ms vs measured %.1f ms", reported, mean));
}

// =================================================================================================================
// [C3] drift
// =================================================================================================================
struct DriftRun { std::vector<double> lat; size_t clicks = 0; StatsLine st; bool sane = false; std::string why; };
DriftRun driftRun(const std::string& file, double ppm, double seconds) {
    DriftRun r;
    char opt[64];
    std::snprintf(opt, sizeof(opt), "@drift=%+.0f@loop", ppm);
    const std::string key = "wav:" + file + opt;
    Rig rig(true);
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(seconds + 0.5, 600000);
    rig.finish();
    const std::vector<size_t> pk = findPeaks(rig.sink.lane[0], 0.05f, 8000);
    r.clicks = pk.size();
    const double rate = 48000.0 * (1.0 + ppm * 1e-6);
    for (size_t k = 0; k < pk.size(); ++k) {
        const double j = 12000.0 + 48000.0 * (double)k;           // click k of the looped file (0.25 + k s)
        r.lat.push_back((double)pk[k] / kAsrRate - (j + 1.0) / rate);
    }
    r.st = lastStats(rig.ev, key, "mix");
    r.sane = rig.sink.sane();
    r.why = rig.sink.why();
    return r;
}

void testC3() {
    const std::string w = media("driftclicks.wav");
    writeWav(w, 48000, 1, clicks(48000, 10.0, 0.25, 1.0, 0.5f), true);
    const double sec = 600.0;
    const DriftRun r0 = driftRun(w, 0, sec), rp = driftRun(w, 500, sec), rm = driftRun(w, -500, sec);
    const size_t want = 600;
    ok("[C3] every click arrives, once, in all three runs (600 each)", r0.clicks >= want && rp.clicks >= want && rm.clicks >= want,
       F("%zu / %zu / %zu", r0.clicks, rp.clicks, rm.clicks));
    auto worst = [&](const DriftRun& d) {
        double w2 = 0;
        for (size_t k = 0; k < want && k < d.lat.size() && k < r0.lat.size(); ++k) w2 = std::max(w2, std::fabs(d.lat[k] - r0.lat[k]));
        return w2 * 1000.0;
    };
    double l0min = 1e9, l0max = -1e9;
    for (size_t k = 0; k < std::min(want, r0.lat.size()); ++k) { l0min = std::min(l0min, r0.lat[k]); l0max = std::max(l0max, r0.lat[k]); }
    info(F("no-drift latency %.1f .. %.1f ms; %s", l0min * 1000, l0max * 1000, statsText(r0.st).c_str()));
    info(F("+500 ppm: %s", statsText(rp.st).c_str()));
    info(F("-500 ppm: %s", statsText(rm.st).c_str()));
    ok("[C3] +500 ppm: every click within +-1 hop (30 ms) of the no-drift run", worst(rp) <= 30.0, F("worst %.1f ms", worst(rp)));
    ok("[C3] -500 ppm: every click within +-1 hop (30 ms) of the no-drift run", worst(rm) <= 30.0, F("worst %.1f ms", worst(rm)));
    // 500 ppm of 10 min = 4800 frames at 16 kHz: minus the band the fill may take up first (160), plus nothing else
    ok("[C3] +500 ppm: ~4800 samples dropped one at a time, none repeated", rp.st.found && rp.st.up >= 4300 && rp.st.up <= 5000 && rp.st.down <= 5,
       statsText(rp.st));
    ok("[C3] -500 ppm: ~4800 samples repeated one at a time, none dropped", rm.st.found && rm.st.down >= 4300 && rm.st.down <= 5000 && rm.st.up <= 5,
       statsText(rm.st));
    ok("[C3] no drift: no slip at all", r0.st.found && r0.st.up + r0.st.down <= 2, statsText(r0.st));
    ok("[C3] nothing dropped, no underrun after priming, no resync (all three)",
       r0.st.dropped + rp.st.dropped + rm.st.dropped == 0 && r0.st.underruns + rp.st.underruns + rm.st.underruns == 0 &&
       r0.st.resyncs + rp.st.resyncs + rm.st.resyncs == 0);
    ok("[C3] sane blocks", r0.sane && rp.sane && rm.sane, r0.why + " / " + rp.why + " / " + rm.why);
}

// =================================================================================================================
// [C4] a gap
// =================================================================================================================
void testC4() {
    const std::string w = media("gapsine.wav");
    writeWav(w, 48000, 1, sine(48000, 5.0, 440.0, 0.25f), true);
    const std::string key = "wav:" + w + "@gap=12:3@loop";
    Rig rig(true);
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(20.0, 60000);
    rig.finish();
    const auto st = rig.ev.statesOf(key);
    ok("[C4] INPUT open -> absent -> open, nothing else", stateSeq(st) == "open>absent>open", stateSeq(st));
    if (st.size() == 3) {
        // fix round RVA-8: the source is REOPENED on RetryPolicy's ladder, as a device is: tries at 12.2 and 13.2 s fail
        // (still unplugged), the one at 15.2 s opens -- not at the gap's end exactly
        const double ta = (double)st[1].frame / kAsrRate, to = (double)st[2].frame / kAsrRate;
        ok("[C4] absent at 12 s, open again at 15.2 s = the ladder's try after the gap (the source's clock)",
           std::fabs(ta - 12.0) < 0.05 && std::fabs(to - 15.2) < 0.05, F("absent at %.3f s, open at %.3f s", ta, to));
    }
    const auto& l = rig.sink.lane[0];
    ok("[C4] silence (exact zeros) through the gap", allZero(l, 12.1, 15.0));
    ok("[C4] the sine is back after it", rmsOf(l, (size_t)(15.3 * kAsrRate), (size_t)(16.0 * kAsrRate)) > 0.15);
    ok("[C4] the mixer never stopped: 20 s of blocks", rig.sink.frames.load() >= (uint64_t)(20.0 * kAsrRate));
    ok("[C4] sane blocks", rig.sink.sane(), rig.sink.why());

    // Two gaps: a stream that stayed healthy RESETS the ladder (fix round RVA-8). Gap 1 at 5 s: tries at 5.2 (fails)
    // and 6.2 (opens); healthy from 11.2 s. Gap 2 at 20 s: from the ladder's FIRST step again -- 20.2 (fails), 21.2
    // (opens). Without the reset it would have reopened at 22.0 (the ladder's third step, 2 s).
    const std::string key2 = "wav:" + w + "@gap=5:1@gap=20:1@loop";
    Rig r2(true);
    r2.hub->start({ strip(key2) }, &r2.sink);
    r2.frames(24.0, 60000);
    r2.finish();
    const auto s2 = r2.ev.statesOf(key2);
    ok("[C4] two gaps: open > absent > open > absent > open", stateSeq(s2) == "open>absent>open>absent>open", stateSeq(s2));
    if (s2.size() == 5) {
        const double o1 = (double)s2[2].frame / kAsrRate, o2 = (double)s2[4].frame / kAsrRate;
        ok("[C4] ... reopened at 6.2 s, then -- the ladder reset by the healthy stream between -- at 21.2 s (not 22.0)",
           std::fabs(o1 - 6.2) < 0.05 && std::fabs(o2 - 21.2) < 0.05, F("open at %.3f s and %.3f s", o1, o2));
    }
}

// =================================================================================================================
// [BACKOFF] a flapping source (fix round RVA-8)
// =================================================================================================================
// @flap=2:20: from 2 s to 22 s every open fails 50 ms after it succeeded. RetryPolicy: the failure at 2.0 s starts the
// episode (ladder step 0: 0.2 s); the reopen at 2.2 s is reported (no flap seen yet) and fails at 2.25 s -- a flap: from
// now on reopens are HELD (INPUT stays "error") and the ladder goes on 1, 2, 5, 5... s, never reset by those 50 ms
// streams: tries at 3.25, 5.30, 10.35, 15.40, 20.45 s (each delivers its 50 ms), then 25.50 s (the flap is over: it
// stays open) and "open" once it ran 5 s healthy: 30.50 s.
void testBackoff() {
    const std::string w = media("flapsine.wav");
    writeWav(w, 48000, 1, sine(48000, 5.0, 440.0, 0.25f), true);
    const std::string key = "wav:" + w + "@flap=2:20@loop";
    Rig rig(true);
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(36.0, 60000);
    rig.finish();
    const auto st = rig.ev.statesOf(key);
    ok("[BACKOFF] INPUT does not flap: open > error > open > error > open (the reopens held while it flaps)",
       stateSeq(st) == "open>error>open>error>open", stateSeq(st));
    if (st.size() == 5) {
        const double tFail = (double)st[1].frame / kAsrRate, tOpen = (double)st[4].frame / kAsrRate;
        ok("[BACKOFF] error at 2.0 s; open again once the stream ran 5 s healthy after the flap: 30.5 s",
           std::fabs(tFail - 2.0) < 0.05 && std::fabs(tOpen - 30.5) < 0.06, F("error at %.3f s, open at %.3f s", tFail, tOpen));
    }
    // The tries, read off the audio: each reopen delivers 50 ms of the sine (the lane plays it ~40 ms later, its fill
    // target); between them, zeros. Their starts, from 2.1 s on.
    const auto& l = rig.sink.lane[0];
    std::vector<double> starts;
    size_t quiet = 1u << 30;
    for (size_t i = (size_t)(2.1 * kAsrRate); i < std::min(l.size(), (size_t)(27.0 * kAsrRate)); ++i) {
        if (std::fabs(l[i]) > 0.02f) {
            if (quiet > (size_t)(0.1 * kAsrRate)) starts.push_back((double)i / kAsrRate);
            quiet = 0;
        } else {
            ++quiet;
        }
    }
    std::string got;
    for (double s : starts) got += F("%.2f ", s);
    bool ladder = starts.size() == 7;
    const double want[] = { 0.0, 1.05, 2.05, 5.05, 5.05, 5.05, 5.05 };      // try-to-try = the wait + the 50 ms it ran
    for (size_t i = 1; ladder && i < starts.size(); ++i) ladder = std::fabs((starts[i] - starts[i - 1]) - want[i]) < 0.03;
    ok("[BACKOFF] the tries follow the ladder 0.2 / 1 / 2 / 5 / 5 s, never reset by an open that failed at once", ladder,
       "audio from " + got);
    ok("[BACKOFF] ... the first one 0.2 s after the failure", !starts.empty() && std::fabs(starts[0] - 2.2) < 0.08,
       starts.empty() ? "none" : F("%.3f s", starts[0]));
    int lines = 0;
    {
        std::lock_guard<std::mutex> lk(rig.ev.mu);
        for (const auto& x : rig.ev.logs)
            if (x.find(key) != std::string::npos && x.find("capture: stats ") != 0 && x.find("capture: closed ") != 0) ++lines;
    }
    ok("[BACKOFF] the log stays short: <= 5 lines for the source's whole flapping episode (opened, the failure, the one "
       "reported reopen, steady again)", lines <= 5, F("%d lines", lines));
    ok("[BACKOFF] sane blocks", rig.sink.sane(), rig.sink.why());
}

// =================================================================================================================
// [TREE] the application root rule (fix round RVA-2 / RVA-4)
// =================================================================================================================
// The child of 'parent' running 'exeLower', from a fresh ToolHelp snapshot (0 = none yet).
uint32_t childNamed(uint32_t parent, const wchar_t* exeLower) {
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    uint32_t found = 0;
    for (BOOL more = ::Process32FirstW(snap, &pe); more && !found; more = ::Process32NextW(snap, &pe))
        if (pe.th32ParentProcessID == parent && cap::lowerW(pe.szExeFile) == exeLower) found = pe.th32ProcessID;
    ::CloseHandle(snap);
    return found;
}

void testTree() {
    using P = cap::ProcessTable::Proc;
    // pid, parent, exe -- and creation times (100 ns units; 0 = unreadable)
    const std::vector<P> procs = {
        { 4, 0, L"System" }, { 1, 4, L"explorer.exe" },
        { 10, 1, L"chrome.exe" }, { 11, 10, L"chrome.exe" }, { 12, 11, L"chrome.exe" },           // a browser + children
        { 20, 1, L"steam.exe" }, { 21, 20, L"Game.exe" },                                          // a launcher's game
        { 30, 1, L"ms-teams.exe" }, { 31, 30, L"msedgewebview2.exe" }, { 32, 31, L"msedgewebview2.exe" },   // new Teams
        { 40, 10, L"Zoom.exe" },                                                                    // a browser started Zoom
        { 50, 51, L"player.exe" }, { 51, 4, L"WmiPrvSE.exe" },                                       // RVA-4: unreadable parent
        { 60, 1, L"app.exe" }, { 61, 60, L"app.exe" },                                              // RVA-4: unreadable child
        { 70, 71, L"tool.exe" }, { 71, 1, L"tool.exe" },                                            // a recycled pid (newer)
        { 80, 1, L"SuVtitle.exe" }, { 81, 80, L"msedgewebview2.exe" }, { 82, 81, L"msedgewebview2.exe" },
    };
    std::map<uint32_t, uint64_t> created = {
        { 4, 1 }, { 1, 10 }, { 10, 100 }, { 11, 110 }, { 12, 120 }, { 20, 200 }, { 21, 210 }, { 30, 300 }, { 31, 310 },
        { 32, 320 }, { 40, 400 }, { 50, 500 }, { 51, 0 }, { 60, 600 }, { 61, 0 }, { 70, 700 }, { 71, 9000 },
        { 80, 800 }, { 81, 810 }, { 82, 820 },
    };
    const cap::ProcessTable t(procs, created);
    ok("[TREE] a browser's audio child -> the browser (the same exe climbs)", t.rootOf(12) == 10 && t.rootOf(11) == 10,
       F("rootOf(12) = %u", t.rootOf(12)));
    ok("[TREE] a game started by a launcher is ITS OWN root, never the launcher", t.rootOf(21) == 21 &&
       t.rootsNamed(L"game.exe", {}).size() == 1 && t.rootsNamed(L"steam.exe", {}).size() == 1,
       F("rootOf(21) = %u", t.rootOf(21)));
    ok("[TREE] new Teams' WebView2 sound -> ms-teams.exe (the WebView2 host), and no msedgewebview2.exe root",
       t.rootOf(32) == 30 && t.rootsNamed(L"ms-teams.exe", {}) == std::vector<uint32_t>{ 30 } &&
       t.rootsNamed(L"msedgewebview2.exe", {}).empty(), F("rootOf(32) = %u", t.rootOf(32)));
    ok("[TREE] a meeting app a browser started is its own root (app:zoom.exe finds it)",
       t.rootOf(40) == 40 && t.rootsNamed(L"zoom.exe", {}) == std::vector<uint32_t>{ 40 });
    ok("[TREE] RVA-4: a parent whose creation time is unreadable is no parent (WmiPrvSE never becomes the app)",
       t.rootOf(50) == 50 && t.rootsNamed(L"player.exe", {}) == std::vector<uint32_t>{ 50 });
    ok("[TREE] RVA-4: ... nor is one when the CHILD's is unreadable (fail safe: two roots, both found by name)",
       t.rootOf(61) == 61 && t.rootsNamed(L"app.exe", {}).size() == 2);
    ok("[TREE] a recycled pid (a 'parent' younger than its child) is no parent", t.rootOf(70) == 70);
    ok("[TREE] the app's own tree, through ANY parent: SuVtitle.exe -> its WebView2 -> its child",
       t.inTreeOf(82, 80) && t.inTreeOf(81, 80) && !t.inTreeOf(12, 80) && !t.inTreeOf(50, 51) &&
       t.rootsNamed(L"suvtitle.exe", { 80 }).empty());

    // ---- the same rule on processes this test starts: cmd (A) -> cmd (B) -> PING.EXE, in a kill-on-close job ----
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (job) ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
    wchar_t sys[MAX_PATH] = {};
    ::GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring cmdExe = std::wstring(sys) + L"\\cmd.exe";
    std::wstring cmd = L"\"" + cmdExe + L"\" /d /c \"\"" + cmdExe + L"\" /d /c \"\"" + std::wstring(sys) +
                       L"\\PING.EXE\" -n 30 127.0.0.1 >nul\"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    const bool started = job && ::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                                 nullptr, nullptr, &si, &pi);
    if (!started) {
        notChecked("[TREE] real processes: cmd.exe could not be started here");
        if (job) ::CloseHandle(job);
        return;
    }
    ::AssignProcessToJobObject(job, pi.hProcess);
    ::ResumeThread(pi.hThread);
    const uint32_t a = pi.dwProcessId;
    uint32_t b = 0, p = 0;
    for (int i = 0; i < 100 && !p; ++i) {
        ::Sleep(50);
        if (!b) b = childNamed(a, L"cmd.exe");
        if (b) p = childNamed(b, L"ping.exe");
    }
    if (!p) {
        notChecked("[TREE] real processes: the cmd -> cmd -> ping chain did not show up within 5 s");
    } else {
        const cap::ProcessTable now;
        const std::vector<uint32_t> pings = now.rootsNamed(L"ping.exe", {});
        ok("[TREE] real: a SAME-name child climbs to its parent (cmd B -> cmd A)", now.rootOf(b) == a,
           F("A %u, B %u: rootOf(B) = %u", a, b, now.rootOf(b)));
        ok("[TREE] real: a DIFFERENT-name child is its own root (ping, started by cmd B) and app:ping.exe finds it",
           now.rootOf(p) == p && std::find(pings.begin(), pings.end(), p) != pings.end(),
           F("ping %u: rootOf = %u, %zu ping root(s)", p, now.rootOf(p), pings.size()));
        ok("[TREE] real: the test (another exe) is not climbed into either", now.rootOf(a) == a);
    }
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(job);                                   // kills A, B and ping: ours, never anyone else's
}

// =================================================================================================================
// [LIST] the picker's list watching has the hub's lifetime (fix round RVA-9)
// =================================================================================================================
void testList() {
    FakeEvents ev;
    FakeSink sink;
    ev.sink = &sink;
    HubOptions o;
    o.testSources = true;
    o.virtualClock = true;
    int afterGone = 0;
    {
        TrCaptureHub hub(&ev, o);
        hub.requestSources();
        hub.stop();                                       // a Listen off racing the picker's request
        const bool answered = waitFor([&] { return ev.listCount() >= 1; }, 10000);
        ok("[LIST] requestSources() then stop() at once: the request is still answered (before: lost)", answered,
           F("%zu list(s)", ev.listCount()));
        const std::string w = media("listsine.wav");
        writeWav(w, 48000, 1, sine(48000, 2.0, 440.0, 0.25f), true);
        hub.start({ strip("wav:" + w + "@loop") }, &sink);
        waitFor([&] { return sink.frames.load() >= 3200; }, 5000);
        hub.stop();                                       // a live run ends: the watching does not
        const size_t n0 = ev.listCount();
        hub.requestSources();
        const bool again = waitFor([&] { return ev.listCount() > n0; }, 10000);
        ok("[LIST] after a live run's stop(), a request is answered as ever (the control thread lives with the hub)", again);
        sink.stopped = true;
    }
    ev.destroyed = true;
    ::Sleep(50);
    afterGone = ev.afterDestroy.load();
    ok("[LIST] the hub destroyed: nothing reaches ILiveEvents after it", afterGone == 0, F("%d", afterGone));
}

// =================================================================================================================
// [C5] an idle source
// =================================================================================================================
void testC5() {
    const std::string w = media("stallsine.wav");
    writeWav(w, 48000, 1, sine(48000, 5.0, 440.0, 0.25f), true);
    const std::string key = "wav:" + w + "@stall=2:30@loop";
    const bool real = !g_quick;
    Rig rig(!real);
    rig.sink.timing = real;
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(34.0, 60000);
    rig.finish();
    const auto st = rig.ev.statesOf(key);
    ok("[C5] INPUT open -> stalled -> open", stateSeq(st) == "open>stalled>open", stateSeq(st));
    if (st.size() == 3) {
        const double ts = (double)st[1].frame / kAsrRate, to = (double)st[2].frame / kAsrRate;
        ok("[C5] stalled 2 s into the silence, open when it delivers again", ts > 3.85 && ts < 4.6 && to > 31.85 && to < 32.4,
           F("stalled at %.2f s, open at %.2f s", ts, to));
    }
    const auto& l = rig.sink.lane[0];
    ok("[C5] zeros all through the stall (2.2 .. 32.0 s)", allZero(l, 2.2, 32.0));
    ok("[C5] the sine is back after it", rmsOf(l, (size_t)(32.4 * kAsrRate), (size_t)(33.4 * kAsrRate)) > 0.15);
    if (real) {
        std::vector<int64_t> q;
        std::vector<uint64_t> at;
        {
            std::lock_guard<std::mutex> lk(rig.sink.mu);
            q = rig.sink.callQpc;
            at = rig.sink.callAt;
        }
        double worst = 0;
        std::vector<double> gaps;
        for (size_t i = 1; i < q.size(); ++i) gaps.push_back((double)(q[i] - q[i - 1]) * 1000.0 / qpcFreq());
        for (double g : gaps) worst = std::max(worst, g);
        std::vector<double> sorted = gaps;
        std::sort(sorted.begin(), sorted.end());
        const double p99 = sorted.empty() ? 0 : sorted[sorted.size() * 99 / 100];
        const double span = q.size() > 1 ? (double)(q.back() - q.front()) / qpcFreq() : 0;
        const double fed = at.size() > 1 ? (double)(at.back() - at.front()) / kAsrRate : 0;
        ok("[C5] a steady cadence: one block per ~10 ms all through (real clock)", worst < 60.0 && p99 < 25.0,
           F("%zu calls, p99 %.1f ms, worst %.1f ms", q.size(), p99, worst));
        ok("[C5] frames fed match the QPC time (within 0.5 %)", span > 30.0 && std::fabs(fed / span - 1.0) < 0.005,
           F("%.3f s fed over %.3f s", fed, span));
    } else {
        notChecked("[C5] the real-clock cadence (--quick)");
    }
    ok("[C5] sane blocks", rig.sink.sane(), rig.sink.why());
}

// =================================================================================================================
// [RESYNC] / [OVF]
// =================================================================================================================
void testResync() {
    const std::string w = media("resyncsine.wav");
    writeWav(w, 48000, 1, sine(48000, 5.0, 440.0, 0.25f), true);
    const std::string key = "wav:" + w + "@drift=+20000@loop";
    Rig rig(true);
    rig.sink.record = false;
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(60.0, 60000);
    rig.finish();
    const StatsLine s = lastStats(rig.ev, key, "mix");
    ok("[RESYNC] a source 2 % fast (beyond what slips absorb): resynced, never overflowing",
       s.found && s.resyncs >= 1 && s.dropped == 0 && s.fill < s.target + 300.0, statsText(s));
}

void testOvf() {
    const std::string w = media("ovfsine.wav");
    writeWav(w, 48000, 1, sine(48000, 5.0, 440.0, 0.25f), true);
    const std::string key = "wav:" + w + "@loop";
    Rig rig(false);
    rig.sink.record = false;
    std::atomic<bool> stalledOnce{ false };
    rig.sink.hook = [&](uint64_t fr) {
        if (fr >= (uint64_t)kAsrRate && !stalledOnce.exchange(true)) ::Sleep(2500);    // the VAD stuck for 2.5 s
    };
    rig.hub->start({ strip(key) }, &rig.sink);
    rig.frames(6.0, 20000);
    rig.finish();
    const StatsLine s = lastStats(rig.ev, key, "mix");
    ok("[OVF] a consumer stalled 2.5 s: the source never waited -- its FIFO dropped the oldest and counted it",
       s.found && s.dropped >= 3000, statsText(s));
    ok("[OVF] the mixer skipped ahead and the lane resynced to its target", rig.ev.hasLog("the mixer was held up") && s.resyncs >= 1 &&
       s.fill <= s.target + 15.0, statsText(s));
}

// =================================================================================================================
// [REAL] this PC
// =================================================================================================================
std::wstring exeDir() {
    wchar_t b[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p(b);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

void testReal() {
    Rig rig(false, false);
    rig.hub->requestSources();
    const bool got = waitFor([&] { return rig.ev.listCount() >= 1; }, 8000);
    ok("[REAL] requestSources answers with the list", got);
    const std::vector<SourceInfo> list = rig.ev.lastList();
    int nin = 0, nout = 0, napp = 0, defOut = 0, defIn = 0;
    bool keysOk = true, unique = true, noSelf = true, inNotOpened = true;
    std::string defOutKey;
    for (size_t i = 0; i < list.size(); ++i) {
        const SourceInfo& s = list[i];
        info(F("source %-3s %-12s rate %6u ch %u flags %-12s %s", s.kind.c_str(), s.state.c_str(), s.rate, s.ch, s.flags.c_str(),
               s.label.c_str()));
        if (trpipe::liveKeyKind(s.key) != s.kind) keysOk = false;
        for (size_t j = 0; j < i; ++j) if (list[j].key == s.key) unique = false;
        if (s.kind == "in") { ++nin; if (s.flags.find("default") != std::string::npos) ++defIn; }
        if (s.kind == "out") { ++nout; if (s.flags.find("default") != std::string::npos) { ++defOut; defOutKey = s.key; } }
        if (s.kind == "app") { ++napp; if (s.key == "app:capture_smoke.exe" || s.key == "app:mut_smoke.exe") noSelf = false; }
    }
    info(F("%d inputs, %d outputs, %d applications", nin, nout, napp));
    ok("[REAL] the list: well-formed unique keys, one default output, at most one default input", keysOk && unique && defOut == 1 && defIn <= 1);
    ok("[REAL] this process's own tree is never listed", noSelf);
    for (const auto& l : rig.ev.logs) if (l.find("capture: opened in:") != std::string::npos) inNotOpened = false;
    ok("[REAL] listing opened no input", inNotOpened);
    rig.finish();

    // ---- the default output's loopback, 3 s ----
    if (defOutKey.empty()) {
        notChecked("[REAL] output loopback: no default output");
    } else {
        Rig r(false, false);
        r.sink.timing = true;
        r.sink.record = false;
        const ULONGLONG t0 = ::GetTickCount64();
        r.hub->start({ strip(defOutKey) }, &r.sink);
        const ULONGLONG opened = ::GetTickCount64() - t0;
        ::Sleep(3000);
        const double delay = r.hub->inputDelayMs(defOutKey);
        r.finish();
        const auto st = r.ev.statesOf(defOutKey);
        std::vector<int64_t> q = r.sink.callQpc;
        double worst = 0;
        for (size_t i = 1; i < q.size(); ++i) worst = std::max(worst, (double)(q[i] - q[i - 1]) * 1000.0 / qpcFreq());
        const double fed = (double)r.sink.frames.load() / kAsrRate;
        info(r.ev.logWith("capture: opened " + defOutKey));
        const StatsLine s = lastStats(r.ev, defOutKey, "mix");
        info("output loopback: " + statsText(s) + F(", inputDelayMs %.1f ms, start() took %llu ms", delay, (unsigned long long)opened));
        ok("[REAL] the default output's loopback opens (INPUT open, nothing else)", stateSeq(st) == "open", stateSeq(st));
        ok("[REAL] ... and feeds 16 kHz blocks steadily for 3 s", fed > 2.8 && fed < 3.3 && worst < 100.0,
           F("%.2f s fed, worst gap %.1f ms", fed, worst));
        ok("[REAL] ... LEVELS ~5 Hz while it runs", r.ev.levels.size() >= 12 && r.ev.levels.size() <= 17, F("%zu", r.ev.levels.size()));
        ok("[REAL] sane blocks", r.sink.sane(), r.sink.why());
    }

    // ---- one application's loopback: capture_tone.exe, started by us, orphaned through a launcher ----
    if (!cap::processLoopbackSupported()) {
        notChecked(F("[REAL] application loopback: Windows build %u < 20348", cap::windowsBuild()));
        return;
    }

    // ---- fix round RVA-5: two SEPARATE roots of one exe -- two PING.EXE this test starts through "cmd /c start /b",
    // whose cmd exits at once (each ping is then its own root, outside this test's own -- hidden -- process tree), in a
    // kill-on-close job, no window. ONE root is captured (the oldest), and the log says the other is not heard. (A ping
    // plays nothing: its loopback is silence -- nothing of anyone else's.) ----
    {
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));
        wchar_t sys[MAX_PATH] = {};
        ::GetSystemDirectoryW(sys, MAX_PATH);
        DWORD pings[2] = { 0, 0 };
        for (int k = 0; k < 2 && job; ++k) {
            std::wstring cmd = L"\"" + std::wstring(sys) + L"\\cmd.exe\" /d /c start \"\" /b \"" + std::wstring(sys) +
                               L"\\PING.EXE\" -n 20 127.0.0.1";
            STARTUPINFOW psi = {};
            psi.cb = sizeof(psi);
            PROCESS_INFORMATION ppi = {};
            if (::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                 nullptr, &psi, &ppi)) {
                ::AssignProcessToJobObject(job, ppi.hProcess);
                ::ResumeThread(ppi.hThread);
                ::WaitForSingleObject(ppi.hProcess, 5000);   // cmd is gone once its ping runs
                for (int i = 0; i < 40 && !pings[k]; ++i) {
                    pings[k] = childNamed(ppi.dwProcessId, L"ping.exe");
                    if (!pings[k]) ::Sleep(25);
                }
                ::CloseHandle(ppi.hThread);
                ::CloseHandle(ppi.hProcess);
            }
            ::Sleep(30);                                      // two distinct creation times: "the oldest" is defined
        }
        if (!pings[0] || !pings[1]) {
            notChecked("[REAL] RVA-5: two PING.EXE could not be started");
        } else {
            Rig r(false, false);
            r.hub->start({ strip("app:ping.exe") }, &r.sink);
            const bool opened = waitFor([&] { return !r.ev.statesOf("app:ping.exe").empty(); }, 6000);
            const std::string line = r.ev.logWith("separate ping.exe processes run");
            const std::string open = r.ev.logWith("capture: opened app:ping.exe");
            const auto st = r.ev.statesOf("app:ping.exe");
            r.finish();
            info(line.empty() ? "no RVA-5 line" : line);
            ok("[REAL] RVA-5: two separate roots of one exe -- the oldest is captured and the log says the other is NOT heard",
               opened && stateSeq(st) == "open" && !line.empty() && line.find(std::to_string(pings[0])) != std::string::npos &&
               line.find(std::to_string(pings[1])) != std::string::npos && line.find("NOT heard") != std::string::npos &&
               open.find("pid " + std::to_string(pings[0])) != std::string::npos,
               stateSeq(st) + "; " + (line.empty() ? "no such log line" : "") + "; " + open);
        }
        if (job) ::CloseHandle(job);                          // the two pings: ours, never anyone else's
    }
    const std::wstring dir = exeDir();
    if (::GetFileAttributesW((dir + L"capture_tone.exe").c_str()) == INVALID_FILE_ATTRIBUTES) {
        notChecked("[REAL] application loopback: capture_tone.exe is not next to the smoke (build_capture_smoke.cmd builds it)");
        return;
    }
    const std::wstring evName = L"Local\\SuVtitleCaptureTone." + std::to_wstring(::GetCurrentProcessId());
    HANDLE go = ::CreateEventW(nullptr, TRUE, FALSE, evName.c_str());
    std::wstring cmd = L"\"" + dir + L"capture_smoke.exe\" --spawn-tone " + evName;
    {
        wchar_t self[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, self, MAX_PATH);
        cmd = L"\"" + std::wstring(self) + L"\" --spawn-tone " + evName;
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    DWORD tonePid = 0;
    if (::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        ::WaitForSingleObject(pi.hProcess, 10000);
        DWORD code = 0;
        ::GetExitCodeProcess(pi.hProcess, &code);
        tonePid = code;
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    HANDLE tone = tonePid ? ::OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, tonePid) : nullptr;
    ok("[REAL] capture_tone.exe started (through a launcher that exited: it is its own root)", tone != nullptr, F("pid %lu", tonePid));
    if (tone) {
        const std::string key = "app:capture_tone.exe";
        Rig r(false, false);
        bool listed = false;
        SourceInfo entry;
        for (int i = 0; i < 20 && !listed; ++i) {
            r.hub->requestSources();
            ::Sleep(300);
            for (const auto& s : r.ev.lastList()) if (s.key == key) { listed = true; entry = s; }
        }
        ok("[REAL] the picker lists it as an application (its root, labelled, state ok)", listed && entry.kind == "app" && entry.state == "ok",
           listed ? entry.label + " / " + entry.state : "not listed");
        r.sink.timing = true;
        r.hub->start({ strip(key) }, &r.sink);
        const auto st0 = r.ev.statesOf(key);
        ok("[REAL] its loopback opens", stateSeq(st0) == "open", stateSeq(st0) + " " + r.ev.logWith("capture: opened app:"));
        info(r.ev.logWith("capture: opened app:"));
        const uint64_t f0 = r.sink.frames.load();
        ::SetEvent(go);
        const bool gone = waitFor([&] { return r.ev.statesOf(key).size() >= 2; }, 6000);
        const uint64_t f1 = r.sink.frames.load();
        ::Sleep(300);
        r.finish();
        const auto st = r.ev.statesOf(key);
        ok("[REAL] when it exits: INPUT absent (it is looked for by name again)", gone && stateSeq(st) == "open>absent",
           stateSeq(st) + (st.size() > 1 ? " -- " + st[1].why : ""));
        // the tone in the lane: its peak, and 440 Hz dominating
        const auto& l = r.sink.lane[0];
        float peak = 0;
        for (size_t i = (size_t)f0; i < std::min<size_t>(l.size(), (size_t)f1); ++i) peak = std::max(peak, std::fabs(l[i]));
        const double p440 = goertzel(l, (size_t)f0, (size_t)f1, 440.0), p1k = goertzel(l, (size_t)f0, (size_t)f1, 1000.0),
                     p300 = goertzel(l, (size_t)f0, (size_t)f1, 300.0);
        const double dbPeak = 20.0 * std::log10(std::max(peak, 1e-9f));
        info(F("captured tone: peak %.1f dBFS; 440 Hz over 1 kHz %.1f dB, over 300 Hz %.1f dB", dbPeak,
               10.0 * std::log10(std::max(p440, 1e-30) / std::max(p1k, 1e-30)), 10.0 * std::log10(std::max(p440, 1e-30) / std::max(p300, 1e-30))));
        ok("[REAL] the -60 dBFS 440 Hz tone is captured (only that process's sound)", dbPeak > -70.0 && dbPeak < -45.0 &&
           p440 > 100.0 * p1k && p440 > 100.0 * p300, F("peak %.1f dBFS", dbPeak));
        if (::WaitForSingleObject(tone, 3000) != WAIT_OBJECT_0) {
            ::TerminateProcess(tone, 1);                      // ours (started by this test), never anyone else's
            info("capture_tone.exe had to be terminated");
        }
        ::CloseHandle(tone);
    }
    if (go) ::CloseHandle(go);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // The launcher of [REAL]: start capture_tone.exe and exit at once with its pid, so the tone process has no
    // living parent (it is its own root, outside this smoke's process tree).
    if (argc >= 3 && std::wcscmp(argv[1], L"--spawn-tone") == 0) {
        std::wstring cmd = L"\"" + exeDir() + L"capture_tone.exe\" " + argv[2];
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (!::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return 0;
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return (int)pi.dwProcessId;
    }
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--quick") == 0) g_quick = true;
        if (std::wcscmp(argv[i], L"--only") == 0 && i + 1 < argc) {
            const std::wstring w = argv[++i];
            g_only.clear();
            for (const wchar_t c : w) g_only.push_back((char)c);       // ASCII tags: the narrowing is explicit
        }
    }
    char cwd[MAX_PATH] = {};
    ::GetCurrentDirectoryA(MAX_PATH, cwd);
    g_media = std::string(cwd) + "\\capture_media\\";
    ::CreateDirectoryA(g_media.c_str(), nullptr);
    std::printf("capture_smoke%s -- Windows build %u\n", g_quick ? " --quick" : "", cap::windowsBuild());

    if (section("C1"))     testC1();
    if (section("ARITH"))  testArith();
    if (section("FIFO"))   testFifo();
    if (section("STATE"))  testState();
    if (section("REFUSE")) testRefuse();
    if (section("LEVELS")) testLevels();
    if (section("SET"))    testSet();
    if (section("STOP"))   testStop();
    if (section("C2"))     { testC2(); if (!g_quick) testC2Latency(); else notChecked("[C2] latency (--quick: real clock only)"); }
    if (section("C3"))     testC3();
    if (section("C4"))     testC4();
    if (section("C5"))     testC5();
    if (section("RESYNC")) testResync();
    if (section("OVF"))    { if (!g_quick) testOvf(); else notChecked("[OVF] (--quick: real clock only)"); }
    if (section("REAL"))   { if (!g_quick) testReal(); else notChecked("[REAL] devices (--quick)"); }
    if (section("TREE"))    testTree();
    if (section("BACKOFF")) testBackoff();
    if (section("LIST"))    testList();

    std::printf("capture_smoke: %d passed, %d FAILED, %d not checked\n", g_pass, g_fail, g_nc);
    return g_fail ? 1 : 0;
}
