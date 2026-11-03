// =============================================================================
// SuVtitle - engine/TrCaptureHub.cpp   (the LIVE capture hub: the engine's own audio, through Windows)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b -- replaces the L0 stub. The contract is engine/TrCaptureApi.h; the design
// is SUVTITLE_LIVE_PLAN section 4 and docs/LIVE_PLAN.md section 4.2 (log: docs/PROGRESS_L1b.md). Clean room:
// Microsoft's documentation only.
//
//   start(inputs)  one LANE per strip (TrCaptureCore.h), one SOURCE per distinct key -- a WASAPI device or
//                  application (TrCaptureWasapi.h), a test WAV file (TrCaptureWav.h) -- each on its own thread. A
//                  replacement keeps every lane whose {key, channel} did not move (its trim changed in place) and
//                  every source still in use, without reopening anything.
//   the MIXER      one thread: every ~10 ms of QPC time (or of the virtual clock) the frames due, in blocks of
//                  <= 160: each lane's fill rule -> clamp -> peak -> sum -> ILiveSink::feed16k. LEVELS every
//                  200 ms; a stats line per lane every minute and at the end (the engine's log).
//   the CONTROL    one thread: the picker's list (requestSources -> onSources, pushed again when it changes) and
//                  Windows' device notifications (which also wake a source waiting to reopen). Fix round RVA-9: it
//                  lives with the HUB, not with a live run -- its own event gate, created at the first need (a start
//                  or a request), ended by the destructor -- so a request is never lost to a concurrent stop() and the
//                  list a picker shows keeps following the devices after a Listen off. The WATCHING (re-enumerating
//                  every 2 s) ends kListWatchMs after the last request: one picker opening no longer costs the rest of
//                  the session; the page asks again each time the picker opens.
//   stop()         the mixer joined (no ILiveSink call after it), the sources joined within a bound (one stuck in a
//                  driver is abandoned, harmlessly), the run's event gate closed: no INPUT / LEVELS / sink call after it.
//                  The list's onSources (and its log lines) come until the hub is DESTROYED -- runHost destroys it
//                  before the servant's doorbell closes.
// =============================================================================
#include "TrCaptureApi.h"
#include "TrCaptureCore.h"
#include "TrCaptureEnum.h"
#include "TrCaptureWasapi.h"
#include "TrCaptureWav.h"

#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstdio>
#include <map>

namespace pbt {

namespace {

using namespace cap;

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// How long start() waits for the sources it opens (a driver taking its time); stop() for a thread to finish.
constexpr DWORD kOpenWaitMs = 6000;
constexpr DWORD kJoinMs = 3000;
// The picker: re-enumerate this often while watching (applications start and stop playing with no notification of
// ours), push a changed list at most this often, and let a burst of device notifications settle first.
constexpr ULONGLONG kListPollMs = 2000;
constexpr ULONGLONG kListMinGapMs = 500;
constexpr ULONGLONG kListSettleMs = 300;
// ... and stop watching this long after the last request (fix round RVA-9): the page asks each time the picker opens.
constexpr ULONGLONG kListWatchMs = 180000;
// LEVELS every 200 ms of the mixer's clock (~5 Hz, TrPipe.h); a stats line per lane every minute.
constexpr int kLevelFrames = kAsrRate / 5;
constexpr uint64_t kStatsFrames = (uint64_t)kAsrRate * 60;
// The mixer held up longer than this (the machine slept, a debugger): it skips ahead instead of bursting.
constexpr int64_t kMaxCatchUpFrames = kAsrRate;

// The name a lane is bound under (bindInputs keys[k] -> LINE.spkSlot, LEVELS, inputDelayMs): its LANE ID (TrPipe.h
// LANES, the director's answer to request R1) -- the key alone for a mix strip, "<key>|<chan>" for a one-channel strip --
// so two strips on two channels of ONE source never share a speaker nor a meter. INPUT stays keyed by the source.
std::string laneName(const Lane& l) { return trpipe::liveLaneId(l.key, l.chan); }

std::string chanText(uint32_t chan) { return chan ? "ch" + std::to_string(chan) : "mix"; }

bool sameList(const std::vector<SourceInfo>& a, const std::vector<SourceInfo>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const SourceInfo& x = a[i];
        const SourceInfo& y = b[i];
        if (x.kind != y.kind || x.key != y.key || x.label != y.label || x.state != y.state || x.rate != y.rate ||
            x.ch != y.ch || x.flags != y.flags)
            return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// The sources the control thread wakes when Windows reports a device change.
// ---------------------------------------------------------------------------------------------------------------
struct KickList {
    std::mutex mu;
    std::vector<std::weak_ptr<Source>> sources;
    void set(std::vector<std::weak_ptr<Source>> v) {
        std::lock_guard<std::mutex> lk(mu);
        sources = std::move(v);
    }
    void kickAll() {
        std::vector<std::weak_ptr<Source>> v;
        {
            std::lock_guard<std::mutex> lk(mu);
            v = sources;
        }
        for (auto& w : v) if (auto s = w.lock()) s->kick();
    }
};

// ---------------------------------------------------------------------------------------------------------------
// IMMNotificationClient. Microsoft's rules for it: non-blocking, never Register/Unregister inside, never the final
// Release of an MMDevice object inside -- so it only signals the control thread. Windows does NOT reference-count it
// (UnregisterEndpointNotificationCallback): one per hub, released by the hub only after its unregistration and once
// no callback is in flight.
// ---------------------------------------------------------------------------------------------------------------
class NotifyClient : public IMMNotificationClient {
public:
    NotifyClient() : ev_(::CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    HANDLE event() const { return ev_; }
    bool idle() const { return inflight_.load() == 0; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return signal(); }
    STDMETHODIMP OnDeviceAdded(LPCWSTR) override { return signal(); }
    STDMETHODIMP OnDeviceRemoved(LPCWSTR) override { return signal(); }
    STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) override { return signal(); }
    STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return signal(); }

private:
    ~NotifyClient() { if (ev_) ::CloseHandle(ev_); }
    HRESULT signal() {
        ++inflight_;
        ::SetEvent(ev_);
        --inflight_;
        return S_OK;
    }
    std::atomic<ULONG> ref_{ 1 };
    std::atomic<int>   inflight_{ 0 };
    HANDLE             ev_;
};

// ---------------------------------------------------------------------------------------------------------------
// The CONTROL thread: the picker's list and the device notifications.
// ---------------------------------------------------------------------------------------------------------------
class Control : public std::enable_shared_from_this<Control> {
public:
    Control(std::shared_ptr<EventGate> gate, std::shared_ptr<KickList> kicks, NotifyClient* client,
            std::vector<uint32_t> hidden)
        : gate_(std::move(gate)), kicks_(std::move(kicks)), client_(client), hidden_(std::move(hidden)) {
        stopEv_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        requestEv_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (client_) client_->AddRef();
    }
    ~Control() {
        if (client_) client_->Release();
        if (stopEv_) ::CloseHandle(stopEv_);
        if (requestEv_) ::CloseHandle(requestEv_);
    }
    void begin() {
        std::shared_ptr<Control> self = shared_from_this();
        thread_.start([self]() { self->threadMain(); });
    }
    void request() { ::SetEvent(requestEv_); }
    void requestStop() { ::SetEvent(stopEv_); }
    bool join(DWORD ms) { return thread_.join(ms); }

private:
    void push(const std::vector<SourceInfo>& list) {
        gate_->sources(list);
        unsigned n[3] = { 0, 0, 0 };
        for (const SourceInfo& s : list) ++n[s.kind == "in" ? 0 : s.kind == "out" ? 1 : 2];
        char b[128];
        std::snprintf(b, sizeof(b), "capture: source list: %u inputs, %u outputs, %u applications", n[0], n[1], n[2]);
        gate_->log(b);
    }

    void threadMain() {
        const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(&en))))
            en = nullptr;
        bool registered = false;
        if (en && client_) registered = SUCCEEDED(en->RegisterEndpointNotificationCallback(client_));
        if (!en) gate_->log("capture: the Windows audio device list is not available");

        bool watching = false, dirty = false;
        ULONGLONG lastPush = 0, dirtyAt = 0, lastRequest = 0;
        std::vector<SourceInfo> last;
        std::string note;
        HANDLE hs[3] = { stopEv_, requestEv_, client_ ? client_->event() : nullptr };
        const DWORD nh = client_ ? 3u : 2u;
        for (;;) {
            DWORD timeout = INFINITE;
            const ULONGLONG now = ::GetTickCount64();
            if (watching && now - lastRequest >= kListWatchMs) {
                watching = false;
                dirty = false;
                gate_->log("capture: source list: no longer watched (no request for " +
                           std::to_string(kListWatchMs / 60000) + " min; the picker asks again when it opens)");
            }
            if (watching) {
                if (dirty) {
                    const ULONGLONG due = std::max(dirtyAt + kListSettleMs, lastPush + kListMinGapMs);
                    timeout = due > now ? (DWORD)(due - now) : 0;
                } else {
                    timeout = (DWORD)kListPollMs;
                }
            }
            const DWORD w = ::WaitForMultipleObjects(nh, hs, FALSE, timeout);
            if (w == WAIT_OBJECT_0) break;
            if (w == WAIT_OBJECT_0 + 1) {
                // An explicit request is always answered, changed or not.
                note.clear();
                last = enumerateSources(en, hidden_, &note);
                if (!note.empty()) gate_->log("capture: " + note);
                push(last);
                lastPush = lastRequest = ::GetTickCount64();
                watching = true;
                dirty = false;
                continue;
            }
            if (w == WAIT_OBJECT_0 + 2) {
                kicks_->kickAll();
                if (watching && !dirty) { dirty = true; dirtyAt = ::GetTickCount64(); }
                continue;
            }
            if (w != WAIT_TIMEOUT || !watching) continue;
            note.clear();
            std::vector<SourceInfo> list = enumerateSources(en, hidden_, &note);
            if (sameList(list, last)) { dirty = false; continue; }
            if (::GetTickCount64() - lastPush < kListMinGapMs) { dirty = true; dirtyAt = ::GetTickCount64(); continue; }
            last.swap(list);
            push(last);
            lastPush = ::GetTickCount64();
            dirty = false;
        }
        if (registered) {
            en->UnregisterEndpointNotificationCallback(client_);
            // A callback that entered before the unregistration may still be signalling: let it leave first.
            for (int i = 0; i < 1000 && !client_->idle(); ++i) ::Sleep(1);
        }
        rel(en);
        if (SUCCEEDED(co)) ::CoUninitialize();
    }

    std::shared_ptr<EventGate>  gate_;
    std::shared_ptr<KickList>   kicks_;
    NotifyClient*               client_;
    const std::vector<uint32_t> hidden_;
    HANDLE                      stopEv_ = nullptr, requestEv_ = nullptr;
    BoundedThread               thread_;
};

// ---------------------------------------------------------------------------------------------------------------
// A device key on the test engine's virtual clock: a device runs on ITS clock, never on a simulated one.
// ---------------------------------------------------------------------------------------------------------------
class NullSource : public Source {
public:
    NullSource(const std::string& key, std::shared_ptr<EventGate> gate, std::string why)
        : Source(key, std::make_shared<DeviceFeed>(nullptr)), gate_(gate), reporter_(key, gate), why_(std::move(why)) {}
    void begin() override {
        gate_->log("capture: " + key_ + ": " + why_);
        reporter_.set("unsupported", why_);
    }
    bool waitFirstOpen(DWORD) override { return true; }
    void requestStop() override {}
    bool join(DWORD) override { return true; }

private:
    std::shared_ptr<EventGate> gate_;
    StateReporter              reporter_;
    std::string                why_;
};

// ---------------------------------------------------------------------------------------------------------------
// The MIXER.
// ---------------------------------------------------------------------------------------------------------------
struct SlotTable {
    std::shared_ptr<Lane>                lane[kMaxLiveInputs];
    ILiveSink*                           sink = nullptr;
    std::vector<std::shared_ptr<Source>> pumped;           // the virtual clock's WAV sources
};

class Mixer : public std::enable_shared_from_this<Mixer> {
public:
    Mixer(bool virtualClock, std::shared_ptr<EventGate> gate) : virtual_(virtualClock), gate_(std::move(gate)) {
        stopEv_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~Mixer() {
        if (thread_.joinable()) thread_.detach();       // only reached on the mixer thread itself (its last owner)
        if (stopEv_) ::CloseHandle(stopEv_);
    }

    void publish(const SlotTable& t) {
        std::lock_guard<std::mutex> lk(mu_);
        pending_ = t;
        ++pendingVer_;
    }
    void begin() {
        std::shared_ptr<Mixer> self = shared_from_this();
        thread_ = std::thread([self]() { self->threadMain(); });
    }
    // Unbounded on purpose: the mixer never calls a driver, only the sink (which returns, by contract). After this,
    // no ILiveSink call runs.
    void stopAndJoin() {
        stop_.store(true);
        ::SetEvent(stopEv_);
        if (!thread_.joinable()) return;
        if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();   // stop() from inside the sink: a misuse
        else thread_.join();
    }

private:
    void threadMain() {
        HANDLE mmcss = nullptr;
        DWORD taskIndex = 0;
        if (!virtual_) mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
        gate_->log(std::string("capture: mixer started (") + (virtual_ ? "virtual clock" : "real clock") + ")");
        if (virtual_) runVirtual();
        else          runReal();
        statsLines();
        if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    }

    void runReal() {
        HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        LARGE_INTEGER f = {}, t0 = {}, now = {};
        ::QueryPerformanceFrequency(&f);
        ::QueryPerformanceCounter(&t0);
        const uint64_t freq = (uint64_t)f.QuadPart;
        uint64_t fed = 0;
        for (;;) {
            DWORD w;
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -100000;                        // 10 ms; the frames themselves come from the QPC
                ::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                HANDLE hs[2] = { stopEv_, timer };
                w = ::WaitForMultipleObjects(2, hs, FALSE, 50);
            } else {
                w = ::WaitForSingleObject(stopEv_, 10);
            }
            if (w == WAIT_OBJECT_0) break;
            adopt();
            ::QueryPerformanceCounter(&now);
            const uint64_t ticks = (uint64_t)(now.QuadPart - t0.QuadPart);
            const uint64_t target = ticks / freq * (uint64_t)kAsrRate + (ticks % freq) * (uint64_t)kAsrRate / freq;
            int64_t due = (int64_t)(target - fed);
            if (due > kMaxCatchUpFrames) {
                gate_->log("capture: the mixer was held up " + fmtMs((double)due * 1000.0 / kAsrRate) + " ms; skipped ahead");
                fed = target - (uint64_t)kFeedMaxFrames;
                due = kFeedMaxFrames;
            }
            while (due > 0) {
                const int n = (int)std::min<int64_t>(due, kFeedMaxFrames);
                block(n);
                fed += (uint64_t)n;
                due -= n;
            }
        }
        if (timer) ::CloseHandle(timer);
    }

    void runVirtual() {
        // Lockstep with the WAV sources and no sleeping: they produce up to the END of the block, then it is mixed.
        uint64_t fed = 0;
        while (!stop_.load(std::memory_order_relaxed)) {
            adopt();
            const double hubSec = (double)(fed + (uint64_t)kFeedMaxFrames) / (double)kAsrRate;
            for (const auto& s : cur_.pumped) s->pump(hubSec);
            block(kFeedMaxFrames);
            fed += (uint64_t)kFeedMaxFrames;
        }
    }

    void adopt() {
        SlotTable t;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (pendingVer_ == curVer_) return;
            t = pending_;
            curVer_ = pendingVer_;
        }
        for (int k = 0; k < (int)kMaxLiveInputs; ++k)
            if (cur_.lane[k] && cur_.lane[k] != t.lane[k]) statsLine(*cur_.lane[k], "removed");
        cur_ = t;
        std::string keys[kMaxLiveInputs];
        bool same = bound_ && cur_.sink == boundSink_;
        for (int k = 0; k < (int)kMaxLiveInputs; ++k) {
            keys[k] = cur_.lane[k] ? laneName(*cur_.lane[k]) : std::string();
            if (keys[k] != boundKeys_[k]) same = false;
            peak_[k] = 0.0f;
        }
        // bindInputs before the first block of a new set, and only when the names (or the sink) changed: a trim
        // change leaves the set as it was (TrCaptureApi.h).
        if (!same && cur_.sink) {
            cur_.sink->bindInputs(keys);
            for (int k = 0; k < (int)kMaxLiveInputs; ++k) boundKeys_[k] = keys[k];
            boundSink_ = cur_.sink;
            bound_ = true;
        }
    }

    void block(int n) {
        const float* per[kMaxLiveInputs] = {};
        std::fill(mix_, mix_ + n, 0.0f);
        for (int k = 0; k < (int)kMaxLiveInputs; ++k) {
            if (!cur_.lane[k]) continue;
            float* b = lane_[k];
            cur_.lane[k]->pull(b, n);
            float pk = peak_[k];
            for (int i = 0; i < n; ++i) {
                const float v = guardSample(b[i]);
                b[i] = v;
                mix_[i] += v;
                const float a = v < 0 ? -v : v;
                if (a > pk) pk = a;
            }
            peak_[k] = pk;
            per[k] = b;
        }
        for (int i = 0; i < n; ++i) mix_[i] = guardSample(mix_[i]);
        if (cur_.sink) cur_.sink->feed16k(mix_, n, per);

        fed_ += (uint64_t)n;
        levelFrames_ += n;
        if (levelFrames_ >= kLevelFrames) {
            levelFrames_ = 0;
            std::vector<std::pair<std::string, float>> v;
            for (int k = 0; k < (int)kMaxLiveInputs; ++k) {
                if (!cur_.lane[k]) continue;
                v.push_back({ laneName(*cur_.lane[k]), peakDbfs(peak_[k]) });
                peak_[k] = 0.0f;
            }
            if (!v.empty()) gate_->levels(v);
        }
        if (fed_ >= nextStats_) {
            nextStats_ = fed_ + kStatsFrames;
            statsLines();
        }
    }

    void statsLine(const Lane& l, const char* why) {
        const Lane::Stats s = l.stats();
        char b[400];
        std::snprintf(b, sizeof(b),
                      ": fed %.2f s, dropped %llu, underruns %llu, slips +%llu/-%llu, resyncs %llu, fill %.1f ms, "
                      "target %.1f ms, period %.1f ms, decimator %.2f ms, delay %.1f ms (%s)",
                      (double)s.fedFrames / (double)kAsrRate, (unsigned long long)s.dropped,
                      (unsigned long long)s.underruns, (unsigned long long)s.slipsUp, (unsigned long long)s.slipsDown,
                      (unsigned long long)s.resyncs, s.fillMs, s.targetMs, s.periodMs, s.decimatorMs, l.delayMs(), why);
        gate_->log("capture: stats " + l.key + " " + chanText(l.chan) + b);
    }
    void statsLines() {
        for (int k = 0; k < (int)kMaxLiveInputs; ++k)
            if (cur_.lane[k]) statsLine(*cur_.lane[k], stop_.load() ? "final" : "periodic");
    }

    const bool                 virtual_;
    std::shared_ptr<EventGate> gate_;
    HANDLE                     stopEv_ = nullptr;
    std::atomic<bool>          stop_{ false };
    std::thread                thread_;

    std::mutex mu_;
    SlotTable  pending_;
    uint64_t   pendingVer_ = 0;

    // the mixer thread's own
    SlotTable   cur_;
    uint64_t    curVer_ = 0;
    std::string boundKeys_[kMaxLiveInputs];
    ILiveSink*  boundSink_ = nullptr;
    bool        bound_ = false;
    float       lane_[kMaxLiveInputs][kFeedMaxFrames] = {};
    float       mix_[kFeedMaxFrames] = {};
    float       peak_[kMaxLiveInputs] = {};
    int         levelFrames_ = 0;
    uint64_t    fed_ = 0;
    uint64_t    nextStats_ = kStatsFrames;
};

} // namespace

// =================================================================================================================
// The hub
// =================================================================================================================
struct TrCaptureHub::Impl {
    ILiveEvents*          events = nullptr;
    HubOptions            opts;
    std::vector<uint32_t> hidden;              // process trees never offered nor targeted: the app's, the engine's

    std::mutex ctlMu;                          // start / stop, whole (they may take seconds; requestSources never)
    std::mutex lifeMu;                         // the gates and the control thread: held for pointer work only
    std::shared_ptr<EventGate> gate;           // the RUN's: INPUT, LEVELS, the sources' and the mixer's log lines
    // Fix round RVA-9: the list's gate and the control thread live with the HUB (created at the first need, ended by
    // the destructor): a live run's stop() neither loses a pending request nor ends the watching.
    std::shared_ptr<EventGate> listGate;
    std::shared_ptr<Control>   control;
    std::shared_ptr<KickList>  kicks = std::make_shared<KickList>();
    NotifyClient*              notify = nullptr;

    // under ctlMu
    std::map<std::string, std::shared_ptr<Source>> sources;
    std::shared_ptr<Lane>  slots[kMaxLiveInputs];
    std::shared_ptr<Mixer> mixer;
    std::atomic<bool>      running{ false };

    // what inputDelayMs reads (any thread, never held up by a start in progress)
    mutable std::mutex                 viewMu;
    std::vector<std::shared_ptr<Lane>> view;

    // The hub's control thread (and its gate), created on first need: a start (the device notifications that wake a
    // source waiting to reopen) or a request (the picker's list).
    void ensureControl(bool request) {
        std::lock_guard<std::mutex> lk(lifeMu);
        if (!listGate) listGate = std::make_shared<EventGate>(events);
        if (!control) {
            control = std::make_shared<Control>(listGate, kicks, notify, hidden);
            control->begin();
        }
        if (request) control->request();
    }
    // The gate of this run, created by its start.
    std::shared_ptr<EventGate> ensureRunGate() {
        ensureControl(false);
        std::lock_guard<std::mutex> lk(lifeMu);
        if (!gate) gate = std::make_shared<EventGate>(events);
        return gate;
    }
    // The destructor's: the control thread joined (bounded), then its gate closed -- nothing reaches ILiveEvents after.
    void endControl() {
        std::shared_ptr<Control> ctl;
        std::shared_ptr<EventGate> g;
        {
            std::lock_guard<std::mutex> lk(lifeMu);
            ctl = control;
            g = listGate;
            control.reset();
            listGate.reset();
        }
        if (ctl) {
            ctl->requestStop();
            if (!ctl->join(kJoinMs) && g) g->log("capture: the control thread is stuck: abandoned");
        }
        if (g) g->close();
    }

    std::shared_ptr<Source> makeSource(const std::string& key, const std::shared_ptr<EventGate>& g) {
        const std::string kind = trpipe::liveKeyKind(key);
        if (kind == "wav") return std::make_shared<WavSource>(key, g, opts.virtualClock);
        if (opts.virtualClock)
            return std::make_shared<NullSource>(key, g, "A device cannot run on the test engine's virtual clock.");
        const WasapiSource::Mode m = kind == "in"  ? WasapiSource::Mode::Input
                                   : kind == "out" ? WasapiSource::Mode::OutputLoopback
                                                   : WasapiSource::Mode::AppLoopback;
        return std::make_shared<WasapiSource>(key, m, g, hidden);
    }

    // Ask every source of `v` to stop, then join them within ONE bound (they wind down in parallel).
    void stopSources(const std::vector<std::shared_ptr<Source>>& v, const std::shared_ptr<EventGate>& g) {
        for (const auto& s : v) s->requestStop();
        const ULONGLONG deadline = ::GetTickCount64() + kJoinMs;
        for (const auto& s : v) {
            const ULONGLONG now = ::GetTickCount64();
            const DWORD left = deadline > now ? (DWORD)(deadline - now) : 0;
            const bool joined = s->join(left);
            if (!g) continue;
            if (joined) g->log("capture: closed " + s->key());
            else        g->log("capture: " + s->key() + ": its thread is stuck in a driver call: abandoned");
        }
    }

    void startLocked(const std::vector<LiveInput>& inputs, ILiveSink* sink) {
        const std::shared_ptr<EventGate> g = ensureRunGate();

        // 1. Lanes: kept by {key, channel} -- slot, FIFO and Decimator untouched, the trim set in place -- the new
        //    ones in free slots, a slot that was empty before a slot just vacated (TrAudio freezes a segment's names
        //    at its opening: a new strip in the slot of one removed mid-sentence would speak under its name).
        std::shared_ptr<Lane> next[kMaxLiveInputs];
        std::vector<const LiveInput*> fresh;
        for (const LiveInput& in : inputs) {
            bool kept = false;
            for (int k = 0; k < (int)kMaxLiveInputs && !kept; ++k) {
                if (!slots[k] || next[k] || slots[k]->key != in.key || slots[k]->chan != in.chan) continue;
                next[k] = slots[k];
                if (slots[k]->gainDb() != in.gainDb) {
                    slots[k]->setGainDb(in.gainDb);
                    g->log("capture: " + in.key + " " + chanText(in.chan) + ": trim " + fmtMs(in.gainDb) + " dB (in place)");
                }
                kept = true;
            }
            if (!kept) fresh.push_back(&in);
        }
        for (const LiveInput* in : fresh) {
            int pick = -1;
            for (int k = 0; k < (int)kMaxLiveInputs && pick < 0; ++k) if (!next[k] && !slots[k]) pick = k;
            for (int k = 0; k < (int)kMaxLiveInputs && pick < 0; ++k) if (!next[k]) pick = k;
            if (pick >= 0) next[pick] = std::make_shared<Lane>(*in, trpipe::liveKeyKind(in->key));
        }

        // 2. Sources: one per key. Kept ones get their new lane list; unused ones stop; new ones are created.
        std::map<std::string, std::vector<std::shared_ptr<Lane>>> byKey;
        for (int k = 0; k < (int)kMaxLiveInputs; ++k) if (next[k]) byKey[next[k]->key].push_back(next[k]);
        std::vector<std::shared_ptr<Source>> leaving, arriving;
        for (auto it = sources.begin(); it != sources.end();) {
            if (!byKey.count(it->first)) { leaving.push_back(it->second); it = sources.erase(it); }
            else ++it;
        }
        for (auto& kv : byKey) {
            auto it = sources.find(kv.first);
            if (it != sources.end()) { it->second->feed().setLanes(kv.second); continue; }
            std::shared_ptr<Source> s = makeSource(kv.first, g);
            s->feed().setLanes(kv.second);
            sources[kv.first] = s;
            arriving.push_back(s);
        }
        stopSources(leaving, g);
        for (const auto& s : arriving) s->begin();
        std::vector<std::weak_ptr<Source>> kickable;
        for (const auto& kv : sources) kickable.push_back(kv.second);
        kicks->set(kickable);

        // 3. The mixer: the new table, applied at its next block (bindInputs first when the names changed).
        SlotTable t;
        for (int k = 0; k < (int)kMaxLiveInputs; ++k) t.lane[k] = next[k];
        t.sink = sink;
        for (const auto& kv : sources) if (kv.second->pumped()) t.pumped.push_back(kv.second);
        if (!mixer) {
            mixer = std::make_shared<Mixer>(opts.virtualClock, g);
            mixer->publish(t);
            mixer->begin();
        } else {
            mixer->publish(t);
        }
        std::vector<std::shared_ptr<Lane>> lanes;
        for (int k = 0; k < (int)kMaxLiveInputs; ++k) {
            slots[k] = next[k];
            if (next[k]) lanes.push_back(next[k]);
        }
        {
            std::lock_guard<std::mutex> lk(viewMu);
            view = lanes;
        }
        running.store(true);

        // 4. "Returns once every input has either opened or reported why not": the new sources' first attempt.
        const ULONGLONG deadline = ::GetTickCount64() + kOpenWaitMs;
        for (const auto& s : arriving) {
            const ULONGLONG now = ::GetTickCount64();
            const DWORD left = deadline > now ? (DWORD)(deadline - now) : 0;
            if (!s->waitFirstOpen(left))
                g->log("capture: " + s->key() + ": still opening after " + std::to_string(kOpenWaitMs / 1000) +
                       " s (a slow driver); it reports when it is done");
        }
    }

    // Returns this run's gate, to be closed once the deferred calls are out.
    std::shared_ptr<EventGate> stopLocked() {
        if (mixer) {
            mixer->stopAndJoin();                      // no ILiveSink call after this line
            mixer.reset();
        }
        std::shared_ptr<EventGate> g;
        {
            std::lock_guard<std::mutex> lk(lifeMu);
            g = gate;
            gate.reset();
        }
        std::vector<std::shared_ptr<Source>> all;
        for (const auto& kv : sources) all.push_back(kv.second);
        stopSources(all, g);
        sources.clear();
        kicks->set({});
        for (auto& s : slots) s.reset();
        {
            std::lock_guard<std::mutex> lk(viewMu);
            view.clear();
        }
        running.store(false);
        return g;
    }
};

TrCaptureHub::TrCaptureHub(ILiveEvents* events, const HubOptions& opts) : impl_(new Impl) {
    impl_->events = events;
    impl_->opts = opts;
    impl_->hidden = { opts.appPid, (uint32_t)::GetCurrentProcessId() };
    impl_->notify = new NotifyClient();
}

TrCaptureHub::~TrCaptureHub() {
    stop();
    impl_->endControl();                       // the list watching (fix round RVA-9): no lock held while it logs
    if (impl_->notify) impl_->notify->Release();
}

bool TrCaptureHub::start(const std::vector<LiveInput>& inputs, ILiveSink* sink) {
    // Refused outright (TrCaptureApi.h): no sink, no inputs or too many, a test key on a product engine. And what
    // trpipe::unpackLive already refuses on the wire, for a caller that skipped it: a key of no known kind, a
    // channel or a trim out of range, the same key twice except on two different channels >= 1.
    if (!sink || inputs.empty() || inputs.size() > kMaxLiveInputs) return false;
    for (size_t i = 0; i < inputs.size(); ++i) {
        const LiveInput& in = inputs[i];
        const std::string kind = trpipe::liveKeyKind(in.key);
        if (kind.empty() || in.key.size() > trpipe::kLiveKeyMax) return false;
        if (in.key.find('|') != std::string::npos) return false;   // the lane separator (fix round RVA-10)
        if (kind == "wav" && !impl_->opts.testSources) return false;
        if (in.chan > trpipe::kLiveMaxChan) return false;
        if (!(in.gainDb >= trpipe::kLiveGainMinDb && in.gainDb <= trpipe::kLiveGainMaxDb)) return false;
        for (size_t j = 0; j < i; ++j)
            if (inputs[j].key == in.key && (inputs[j].chan == in.chan || inputs[j].chan == 0 || in.chan == 0)) return false;
    }
    EventGate::Defer defer;                    // FIRST: our own ILiveEvents calls go out after the lock is released
    std::lock_guard<std::mutex> lk(impl_->ctlMu);
    impl_->startLocked(inputs, sink);
    return true;
}

void TrCaptureHub::stop() {
    std::shared_ptr<EventGate> closing;
    {
        EventGate::Defer defer;
        std::lock_guard<std::mutex> lk(impl_->ctlMu);
        closing = impl_->stopLocked();
    }
    if (closing) closing->close();             // from here on, no ILiveEvents call runs or will run
}

bool TrCaptureHub::running() const { return impl_->running.load(); }

void TrCaptureHub::requestSources() {
    // Never blocks: lifeMu is only ever held for pointer work; the control thread enumerates and answers -- whatever a
    // live run does meanwhile (fix round RVA-9: a stop() no longer ends the thread that holds the request).
    impl_->ensureControl(true);
}

// By LANE ID (laneName above): the pipeline asks with LINE.spkSlot, which names the dominant strip that way -- a mix
// strip's is its bare key, so a caller holding a key still finds that strip.
double TrCaptureHub::inputDelayMs(const std::string& lane) const {
    std::lock_guard<std::mutex> lk(impl_->viewMu);
    for (const auto& l : impl_->view) if (laneName(*l) == lane) return l->delayMs();
    return 0.0;
}

} // namespace pbt
