// =============================================================================
// SuVtitle - engine/TrCaptureWasapi.cpp   (a Windows audio source -- see TrCaptureWasapi.h)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b. Clean room: written from Microsoft's documentation -- Capturing a Stream,
// Loopback Recording, IAudioClient::Initialize and AUDCLNT_STREAMFLAGS_XXX, IAudioCaptureClient::GetBuffer,
// Recovering from an Invalid-Device Error, ActivateAudioInterfaceAsync, AUDIOCLIENT_ACTIVATION_PARAMS /
// AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS / PROCESS_LOOPBACK_MODE and the Application Loopback sample,
// IAudioSessionControl2::SetDuckingPreference, AvSetMmThreadCharacteristics -- never from another product's code.
// =============================================================================
#include <initguid.h>              // first: PKEY_Device_FriendlyName is DEFINED (selectany) in this translation unit
#include "TrCaptureWasapi.h"
#include "TrCaptureEnum.h"

#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cstdio>

namespace pbt {
namespace cap {

namespace {

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Reopening a lost device: RetryPolicy's ladder (TrCaptureCore.h) -- 0.2 s (a format change re-opens at once), then 1, 2,
// 5 s, then every 5 s, reset only by a stream that stayed healthy.
// An application that is not running (or closed): looked for again by name this often.
constexpr DWORD kAppRetryMs = 2000;
// The privacy settings refuse the microphone: the user may change them; no hurry to ask again.
constexpr DWORD kDeniedRetryMs = 5000;
// An INPUT that delivers no packet for this long is "stalled" (a capture endpoint always delivers -- silence is
// a packet flagged SILENT -- so nothing at all means the driver stopped). Loopbacks deliver nothing when nothing
// plays: no such rule for them.
constexpr ULONGLONG kStallMs = 2000;
// How long Windows may take to answer an application-capture activation.
constexpr DWORD kActivateTimeoutMs = 5000;

// ---- the completion handler of ActivateAudioInterfaceAsync ------------------------------------------------------
// Called by Windows on an MTA worker thread; agile (IAgileObject), as the API requires of it before Windows 10 and
// as costs nothing after. Windows keeps its own reference until the operation completes, so a device thread that
// gave up waiting (stop, timeout) may release ours: the late completion then lands here and is released with it.
class ActivateHandler : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivateHandler() : done_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *ppv = static_cast<IAgileObject*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT hrAct = E_FAIL;
        IUnknown* unk = nullptr;
        const HRESULT hr = op ? op->GetActivateResult(&hrAct, &unk) : E_POINTER;
        {
            std::lock_guard<std::mutex> lk(mu_);
            result_ = FAILED(hr) ? hr : hrAct;
            if (SUCCEEDED(result_) && unk) { unk_ = unk; unk = nullptr; }
        }
        if (unk) unk->Release();
        ::SetEvent(done_);
        return S_OK;
    }

    HANDLE done() const { return done_; }
    HRESULT take(IAudioClient** out) {
        std::lock_guard<std::mutex> lk(mu_);
        if (FAILED(result_)) return result_;
        if (!unk_) return E_UNEXPECTED;
        return unk_->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(out));
    }

private:
    ~ActivateHandler() {
        if (unk_) unk_->Release();
        if (done_) ::CloseHandle(done_);
    }
    std::atomic<ULONG> ref_{ 1 };
    std::mutex mu_;
    HRESULT    result_ = E_PENDING;
    IUnknown*  unk_ = nullptr;
    HANDLE     done_;
};

std::string friendlyName(IMMDevice* d) {
    std::string s;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        PROPVARIANT v;
        ::PropVariantInit(&v);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal) s = utf8(v.pwszVal);
        ::PropVariantClear(&v);
    }
    rel(ps);
    return s;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
WasapiSource::WasapiSource(const std::string& key, Mode mode, std::shared_ptr<EventGate> gate, std::vector<uint32_t> hidden)
    : Source(key, std::make_shared<DeviceFeed>([gate](const std::string& l) { gate->log(l); })),
      mode_(mode), gate_(gate), reporter_(key, gate), hidden_(std::move(hidden)) {
    const size_t colon = key.find(':');
    target_ = wide(colon == std::string::npos ? std::string() : key.substr(colon + 1));
    if (mode_ == Mode::AppLoopback) target_ = lowerW(target_);
    stopEv_  = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    kickEv_  = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    firstEv_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    audioEv_ = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // in: -- a session of this source's own (every reopen joins it again, with its ducking preference)
    if (mode_ == Mode::Input && FAILED(::CoCreateGuid(&sessionGuid_))) sessionGuid_ = GUID_NULL;
}

WasapiSource::~WasapiSource() {
    // Only ever destroyed once its thread is over (the thread holds a shared_ptr to it): nothing of COM is left.
    for (HANDLE h : { stopEv_, kickEv_, firstEv_, audioEv_ }) if (h) ::CloseHandle(h);
}

void WasapiSource::begin() {
    std::shared_ptr<WasapiSource> self = shared_from_this();
    thread_.start([self]() { self->threadMain(); });
}

bool WasapiSource::waitFirstOpen(DWORD ms) { return ::WaitForSingleObject(firstEv_, ms) == WAIT_OBJECT_0; }
void WasapiSource::requestStop() { ::SetEvent(stopEv_); }
bool WasapiSource::join(DWORD ms) { return thread_.join(ms); }
void WasapiSource::kick() { ::SetEvent(kickEv_); }

bool WasapiSource::waitRetry(DWORD ms) {
    HANDLE hs[2] = { stopEv_, kickEv_ };
    const DWORD w = ::WaitForMultipleObjects(2, hs, FALSE, ms);
    if (w == WAIT_OBJECT_0) return false;
    if (w == WAIT_OBJECT_0 + 1) {
        // A device came or changed: try again soon, but give the endpoint a moment to settle (and a burst of
        // notifications one try, not ten).
        return ::WaitForSingleObject(stopEv_, RetryPolicy::ladderMs(0)) != WAIT_OBJECT_0;
    }
    return true;
}

void WasapiSource::threadMain() {
    const HRESULT co = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // MMCSS: the audio engine's own scheduling class. A device thread that is late loses packets (its endpoint
    // buffer is the engine's minimum: shared + event-driven = durations 0, per IAudioClient::Initialize).
    DWORD taskIndex = 0;
    HANDLE mmcss = ::AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
    if (!mmcss) log("MMCSS refused (error " + std::to_string(::GetLastError()) + "): normal priority");
    if (mode_ != Mode::AppLoopback &&
        FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en_))))
        en_ = nullptr;

    // Fix round RVA-8: the back-off, the INPUT held through a flapping episode and the log's rate limit are
    // RetryPolicy's (TrCaptureCore.h) -- a device that opens then fails at once no longer cycles every 0.2 s with four
    // log lines and two INPUT messages per cycle.
    for (;;) {
        if (::WaitForSingleObject(stopEv_, 0) == WAIT_OBJECT_0) break;
        Outcome o = mode_ == Mode::AppLoopback ? openApp() : openEndpoint();
        if (o.ok) {
            if (retry_.opened(::GetTickCount64())) {
                reporter_.set("open", "");
                gate_->log(openLine_);
            }
            ::SetEvent(firstEv_);
            o = runStream();
        }
        closeStream();
        if (o.stopped) break;
        reporter_.set(o.state, o.why);
        ::SetEvent(firstEv_);
        const RetryPolicy::Failure f = retry_.failed(::GetTickCount64(), o.state + "|" + o.why + "|" + o.detail, o.retryMs);
        if (f.log)
            log(o.state + " -- " + o.why + (o.detail.empty() ? "" : " [" + o.detail + "]") + (o.permanent ? "" : " (retrying)"));
        else if (f.still)
            log("still " + o.state + " after " + std::to_string(f.tries) + " tries");
        if (o.permanent) {
            ::WaitForSingleObject(stopEv_, INFINITE);
            break;
        }
        if (!waitRetry(f.waitMs)) break;
    }
    closeStream();
    rel(en_);
    if (mmcss) ::AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(co)) ::CoUninitialize();
}

WasapiSource::Outcome WasapiSource::failure(HRESULT hr, const char* stage) {
    Outcome o;
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
    case AUDCLNT_E_RESOURCES_INVALIDATED:
    case E_NOTFOUND:
        o.state = "absent";
        o.why = "The device was unplugged or turned off; it is reopened as soon as it is back.";
        break;
    case AUDCLNT_E_DEVICE_IN_USE:
        o.state = "busy";
        o.why = "Another application is using this device exclusively.";
        break;
    case E_ACCESSDENIED:
        if (mode_ == Mode::Input) {
            // Not documented by Microsoft as THE code of a privacy refusal (searched 25/10): the plausible one.
            o.state = "denied";
            o.why = "Windows blocks microphone access for desktop apps (Settings > Privacy > Microphone).";
            o.retryMs = kDeniedRetryMs;
        } else {
            o.state = "error";
            o.why = "Windows refused access to this sound (" + hr8(hr) + ").";
        }
        break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        o.state = "error";
        o.why = "The Windows audio service is not running.";
        break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        o.state = "error";
        o.why = "Windows cannot deliver this sound in a usable format.";
        break;
    default:
        o.state = "error";
        o.why = std::string("Windows refused to ") + stage + " it (" + hr8(hr) + ").";
        break;
    }
    o.detail = std::string(stage) + ": " + hr8(hr);      // for the bug report; the sentence above is for the user
    return o;
}

WasapiSource::Outcome WasapiSource::openEndpoint() {
    Outcome o;
    if (!en_) { o.state = "error"; o.why = "The Windows audio device list is not available."; return o; }
    IMMDevice* dev = nullptr;
    HRESULT hr = en_->GetDevice(target_.c_str(), &dev);
    if (FAILED(hr) || !dev) {
        o.state = "absent";
        o.why = "This device is not connected; it is opened as soon as it is.";
        return o;
    }
    DWORD st = 0;
    if (FAILED(dev->GetState(&st)) || st != DEVICE_STATE_ACTIVE) {
        rel(dev);
        o.state = "absent";
        o.why = st == DEVICE_STATE_DISABLED ? "This device is disabled in Windows' sound settings."
              : st == DEVICE_STATE_UNPLUGGED ? "Nothing is plugged into this device's jack."
              : "This device is not connected; it is opened as soon as it is.";
        return o;
    }
    // The key's kind must match the endpoint's direction: "in:" a capture endpoint, "out:" a render one (whose
    // LOOPBACK is captured -- Initialize refuses LOOPBACK on a capture endpoint anyway: AUDCLNT_E_WRONG_ENDPOINT_TYPE).
    IMMEndpoint* ep = nullptr;
    EDataFlow flow = eAll;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IMMEndpoint), reinterpret_cast<void**>(&ep))) && ep) ep->GetDataFlow(&flow);
    rel(ep);
    const EDataFlow want = mode_ == Mode::Input ? eCapture : eRender;
    if (flow != want) {
        rel(dev);
        o.state = "error";
        o.why = mode_ == Mode::Input ? "This device is an output, not an input." : "This device is an input, not an output.";
        o.permanent = true;
        return o;
    }
    name_ = friendlyName(dev);
    if (mode_ == Mode::Input) optOutOfDucking(dev);      // BEFORE the stream exists (fix round RVA-3)
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client_));
    rel(dev);
    if (FAILED(hr) || !client_) return failure(hr, "activate");

    WAVEFORMATEX* mix = nullptr;
    hr = client_->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) return failure(hr, "read the format of");
    SampleFormat mixFmt;
    std::string why;
    const bool mixOk = parseFormat(mix, sizeof(WAVEFORMATEX) + mix->cbSize, mixFmt, why);
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (mode_ == Mode::OutputLoopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0u);
    WAVEFORMATEXTENSIBLE ask = {};
    const WAVEFORMATEX* use = mix;
    size_t useCb = sizeof(WAVEFORMATEX) + mix->cbSize;
    const char* asked = nullptr;
    if (!mixOk || mix->nSamplesPerSec < (DWORD)kAsrRate) {
        // Below 16 kHz (a hands-free Bluetooth microphone runs at 8 kHz): ask Windows for 16 kHz float and let it
        // convert. The Decimator's cut-off is drawn from ITS output Nyquist; fed a lower rate it would be an
        // upsampler with images (TrDecimator.h). A mix format we cannot decode: float at its own rate, converted.
        const uint32_t r = mix->nSamplesPerSec < (DWORD)kAsrRate ? (uint32_t)kAsrRate : (uint32_t)mix->nSamplesPerSec;
        ask = floatFormat(r, std::max<uint32_t>(1, mix->nChannels));
        use = &ask.Format;
        useCb = sizeof(ask);
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        asked = mix->nSamplesPerSec < (DWORD)kAsrRate ? "asked at 16 kHz (Windows converts)" : "asked as float (Windows converts)";
    }
    // Shared + event-driven: both durations 0 -- Initialize's documented rule; the engine sizes the buffer. An input's
    // stream joins the session that already carries its ducking opt-out (optOutOfDucking: AudioSessionGuid).
    const bool ownSession = mode_ == Mode::Input && !IsEqualGUID(sessionGuid_, GUID_NULL);
    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, use, ownSession ? &sessionGuid_ : nullptr);
    const bool parsed = SUCCEEDED(hr) && parseFormat(use, useCb, fmt_, why);
    ::CoTaskMemFree(mix);
    if (FAILED(hr)) return failure(hr, "open");
    if (!parsed) { o.state = "error"; o.why = "Windows delivers this sound in a format SuVtitle cannot read (" + why + ")."; return o; }

    if (mode_ == Mode::Input && !duckDone_) {
        // The session manager refused the opt-out: the same call on the stream's own session, still BEFORE it starts
        // (SetDuckingPreference's remarks: "during stream creation"; the 1.1.0 L1b way).
        IAudioSessionControl* sc = nullptr;
        IAudioSessionControl2* sc2 = nullptr;
        HRESULT dh = client_->GetService(__uuidof(IAudioSessionControl), reinterpret_cast<void**>(&sc));
        if (SUCCEEDED(dh) && sc) dh = sc->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&sc2));
        if (SUCCEEDED(dh) && sc2) dh = sc2->SetDuckingPreference(TRUE);
        if (FAILED(dh)) log("SetDuckingPreference failed on the stream too: " + hr8(dh));
        rel(sc2);
        rel(sc);
    }
    return startStream(asked);
}

// Fix round RVA-3. Microsoft, "Disabling the Default Ducking Experience": "Programmatically, a direct WASAPI client can
// opt out by using the session manager for the audio session that provides volume control for the non-communication
// streams" -- its steps 1-5: the endpoint, its IAudioSessionManager2, the session's IAudioSessionControl, QueryInterface
// IAudioSessionControl2, SetDuckingPreference(TRUE) -- and "the opt-out change does not take effect until the stream is
// stopped and started again": so it is made on OUR session BEFORE our stream joins it (Initialize's AudioSessionGuid)
// and starts. IAudioSessionManager::GetAudioSessionControl: "If the GUID does not identify a session that has been
// previously opened, the call opens a new but empty session"; SetDuckingPreference's remarks: "We recommend that the
// application call SetDuckingPreference during stream creation." The control is held while the stream runs, so the
// session that carries the preference is the one the stream joins.
// /!\ NOT DOCUMENTED, AND NOT VERIFIED HERE (no microphone may be opened by a test): that a CAPTURE stream's opt-out
// keeps the OTHER sounds from being lowered. The page speaks of the session of the "non-communication streams" (a media
// application sparing its OWN sounds), and "Using a Communication Device" says "The operating system considers the
// stream opened on a communication device to be a communication stream" -- the stream whose opening ducks the others.
// The user verifies it (LIVE_PLAN ADDENDUM: his headset microphone as a strip during a video: the video must not get
// quieter); the log below says whether this input is Windows' default communications input, the case that ducks.
void WasapiSource::optOutOfDucking(IMMDevice* dev) {
    duckDone_ = false;
    rel(duckCtl_);
    if (IsEqualGUID(sessionGuid_, GUID_NULL)) return;
    IAudioSessionManager2* sm = nullptr;
    IAudioSessionControl* sc = nullptr;
    HRESULT hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&sm));
    if (SUCCEEDED(hr) && sm) hr = sm->GetAudioSessionControl(&sessionGuid_, 0, &sc);
    if (SUCCEEDED(hr) && sc) hr = sc->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&duckCtl_));
    if (SUCCEEDED(hr) && duckCtl_) hr = duckCtl_->SetDuckingPreference(TRUE);
    duckDone_ = SUCCEEDED(hr) && duckCtl_;
    if (!duckDone_) rel(duckCtl_);
    rel(sc);
    rel(sm);
    if (duckLogged_) return;
    duckLogged_ = true;
    bool comm = false;
    IMMDevice* d = nullptr;
    if (en_ && SUCCEEDED(en_->GetDefaultAudioEndpoint(eCapture, eCommunications, &d)) && d) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(d->GetId(&id)) && id) { comm = target_ == id; ::CoTaskMemFree(id); }
    }
    rel(d);
    log(std::string("ducking: ") + (duckDone_ ? "opted out on its own session before the stream (the session manager)"
                                              : "the session manager refused the opt-out (" + hr8(hr) + "): asked on the stream")
        + (comm ? "; this is Windows' default COMMUNICATIONS input -- whether other sounds still get lowered while it is "
                  "open is Windows' (Sound settings > Communications)"
                : "; not the default communications input"));
}

WasapiSource::Outcome WasapiSource::startStream(const char* what) {
    REFERENCE_TIME period = 0;
    UINT32 bufFrames = 0;
    client_->GetDevicePeriod(&period, nullptr);           // may fail on the virtual loopback device: then 0
    client_->GetBufferSize(&bufFrames);
    HRESULT hr = client_->SetEventHandle(audioEv_);
    if (FAILED(hr)) return failure(hr, "open");
    hr = client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture_));
    if (FAILED(hr) || !capture_) return failure(hr, "open");
    hr = client_->Start();
    if (FAILED(hr)) return failure(hr, "start");
    std::string line = "capture: opened " + key_;
    if (!name_.empty()) line += " (" + name_ + ")";
    if (mode_ == Mode::AppLoopback) {
        line += " -> " + utf8(procExe_) + " pid " + std::to_string(procPid_) + " with its child processes";
    }
    line += ": " + describe(fmt_);
    if (period) line += "; engine period " + fmtMs((double)period / 10000.0) + " ms";
    if (bufFrames && fmt_.rate) line += "; buffer " + fmtMs((double)bufFrames * 1000.0 / (double)fmt_.rate) + " ms";
    if (what) line += std::string("; ") + what;
    openLine_ = line;                // logged by threadMain -- unless RetryPolicy holds a flapping reopen back
    Outcome o;
    o.ok = true;
    return o;
}

HRESULT WasapiSource::activateProcessLoopback(uint32_t pid, bool& stopped) {
    stopped = false;
    AUDIOCLIENT_ACTIVATION_PARAMS ap = {};
    ap.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    ap.ProcessLoopbackParams.TargetProcessId = pid;
    ap.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv;
    ::PropVariantInit(&pv);
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(ap);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&ap);     // our stack: never PropVariantClear'ed
    ActivateHandler* h = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h, &op);
    if (SUCCEEDED(hr)) {
        HANDLE hs[2] = { stopEv_, h->done() };
        const DWORD w = ::WaitForMultipleObjects(2, hs, FALSE, kActivateTimeoutMs);
        if (w == WAIT_OBJECT_0)          { stopped = true; hr = E_ABORT; }
        else if (w == WAIT_OBJECT_0 + 1) hr = h->take(&client_);
        else                             hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    rel(op);
    h->Release();
    return hr;
}

WasapiSource::Outcome WasapiSource::openApp() {
    Outcome o;
    const std::string exe = utf8(target_);
    if (!processLoopbackSupported()) {
        o.state = "unsupported";
        o.why = "Capturing one application needs Windows 11 (or Windows 10 build 20348 or later).";
        o.permanent = true;
        return o;
    }
    const ProcessTable pt;
    const std::vector<uint32_t> roots = pt.rootsNamed(target_, hidden_);
    if (roots.empty()) {
        o.state = "absent";
        o.why = exe + " is not running; SuVtitle looks for it every 2 s.";
        o.retryMs = kAppRetryMs;
        return o;
    }
    // ONE root per strip (fix round RVA-5, the limit documented in TrCaptureWasapi.h): two SEPARATE processes of this exe
    // -- two windows of a player started apart, two browser profiles -- are two roots, and the oldest is captured. Said
    // in the log whenever their number changes, so a silent strip can be explained.
    const uint32_t pid = roots.front();
    if (roots.size() != rootsLogged_) {
        if (roots.size() > 1) {
            std::string pids;
            for (uint32_t r : roots) pids += (pids.empty() ? "" : ", ") + std::to_string(r);
            log(std::to_string(roots.size()) + " separate " + exe + " processes run (pids " + pids + "): the oldest, pid "
                + std::to_string(pid) + ", is captured with its child processes -- the other(s) are NOT heard "
                "(one application capture per strip)");
        }
        rootsLogged_ = roots.size();
    }
    const ProcessTable::Proc* p = pt.find(pid);
    procExe_ = p ? p->exe : target_;
    procPid_ = pid;
    proc_ = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc_) {
        o.state = "absent";
        o.why = exe + " could not be reached; SuVtitle looks for it every 2 s.";
        o.retryMs = kAppRetryMs;
        return o;
    }
    // Float 48 kHz stereo, Windows converting (AUTOCONVERTPCM): the Application Loopback sample fixes its own format
    // the same way (16-bit 44.1 kHz stereo) and notes GetMixFormat as the other road. Should a Windows refuse ours,
    // the sample's own format is the fallback -- on a NEW client, as a failed Initialize requires.
    const WAVEFORMATEXTENSIBLE fx = floatFormat(48000, 2);
    const WAVEFORMATEX pcm = pcm16Format(44100, 2);
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool stopped = false;
        HRESULT hr = activateProcessLoopback(pid, stopped);
        if (stopped) { o.stopped = true; return o; }
        if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
            o.state = "error";
            o.why = "Windows did not answer the request to capture " + exe + ".";
            return o;
        }
        if (FAILED(hr) || !client_) return failure(hr, "capture");
        const WAVEFORMATEX* use = attempt == 0 ? &fx.Format : &pcm;
        const size_t useCb = attempt == 0 ? sizeof(fx) : sizeof(pcm);
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, use, nullptr);
        if (SUCCEEDED(hr)) {
            std::string why;
            if (!parseFormat(use, useCb, fmt_, why)) { o.state = "error"; o.why = "Unusable capture format (" + why + ")."; return o; }
            return startStream(attempt == 0 ? nullptr : "the sample's 16-bit format");
        }
        rel(client_);
        if (attempt == 0 && (hr == AUDCLNT_E_UNSUPPORTED_FORMAT || hr == E_INVALIDARG)) {
            log("float 48 kHz refused (" + hr8(hr) + "): trying 16-bit 44.1 kHz");
            continue;
        }
        return failure(hr, "open");
    }
    o.state = "error";
    o.why = "Windows refused every capture format for " + exe + ".";
    return o;
}

WasapiSource::Outcome WasapiSource::runStream() {
    HANDLE hs[3] = { stopEv_, audioEv_, proc_ };
    const DWORD n = proc_ ? 3 : 2;
    ULONGLONG lastPacket = ::GetTickCount64(), lastDiscLog = 0;
    uint64_t disc = 0, discLogged = 0;
    bool stalled = false;
    for (;;) {
        // 100 ms: a missed event (a loopback before Windows 10 1703 gets none) costs a short delay, never data --
        // every wake drains whatever the buffer holds.
        const DWORD w = ::WaitForMultipleObjects(n, hs, FALSE, 100);
        if (w == WAIT_OBJECT_0) { Outcome o; o.stopped = true; return o; }
        if (w == WAIT_OBJECT_0 + 2) {
            Outcome o;
            o.state = "absent";
            o.why = utf8(target_) + " has closed; SuVtitle looks for it every 2 s.";
            o.retryMs = kAppRetryMs;
            return o;
        }
        if (w == WAIT_FAILED) return failure(HRESULT_FROM_WIN32(::GetLastError()), "wait on");
        for (;;) {
            UINT32 packet = 0;
            HRESULT hr = capture_->GetNextPacketSize(&packet);
            if (FAILED(hr)) return failure(hr, "read");
            if (packet == 0) break;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) break;
            if (FAILED(hr)) return failure(hr, "read");
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data;
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++disc;
            // Copied out, then released at once: the engine expects the packet back within its period.
            if (frames && !silent) {
                conv_.resize((size_t)frames * fmt_.channels);
                toFloat(data, frames, fmt_, conv_.data());
            }
            hr = capture_->ReleaseBuffer(frames);
            if (FAILED(hr)) return failure(hr, "read");
            if (!frames) continue;
            if (silent) feed_->pushSilence(frames, fmt_.channels, fmt_.rate);
            else        feed_->push(conv_.data(), frames, fmt_.channels, fmt_.rate);
            lastPacket = ::GetTickCount64();
            if (stalled) {
                stalled = false;
                log("delivering again");
                reporter_.set("open", "");
            }
        }
        const ULONGLONG now = ::GetTickCount64();
        bool reportOpen = false;
        if (retry_.healthy(now, reportOpen)) {
            // A failing episode is over: the stream ran RetryPolicy::kHealthyMs without failing.
            if (reportOpen) {
                reporter_.set("open", "");
                gate_->log(openLine_ + " -- steady again after a flapping episode");
            } else {
                log("steady again");
            }
        }
        if (mode_ == Mode::Input && !stalled && now - lastPacket > kStallMs) {
            stalled = true;
            log("no packet for 2 s (stalled)");
            reporter_.set("stalled", "No sound arrives from this input; Windows or its driver may have paused it.");
        }
        if (disc != discLogged && now - lastDiscLog >= 10000) {
            // The first packet of a stream usually carries the flag (nothing came before it); more is lost audio.
            log(std::to_string(disc) + " discontinuities so far (audio Windows could not deliver in time)");
            discLogged = disc;
            lastDiscLog = now;
        }
    }
}

void WasapiSource::closeStream() {
    if (client_) client_->Stop();
    rel(capture_);
    rel(client_);
    rel(duckCtl_);
    if (proc_) { ::CloseHandle(proc_); proc_ = nullptr; }
}

} // namespace cap
} // namespace pbt
