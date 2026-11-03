// =============================================================================
// SuVtitle - engine/TrCaptureWasapi.h   (a Windows audio source: an input, an output's sound, one application's)
//
// SuVtitle 1.1.0 (2026-10-25), wave L1b (SUVTITLE_LIVE_PLAN sections 4.1, 4.2, 4.6). One WasapiSource per opened
// source, whatever the number of strips on it; ONE thread each, which does everything WASAPI asks to be done on one
// thread (open, GetBuffer/ReleaseBuffer, the final Release of what GetService returned):
//   in:<endpoint id>   a capture endpoint, shared mode, event-driven; its stream joins a session of its own that opted
//                      out of Windows' communications ducking BEFORE the stream existed ("Disabling the Default Ducking
//                      Experience": the session manager's IAudioSessionControl2::SetDuckingPreference(TRUE), fix round
//                      RVA-3 -- whether that also spares the OTHER sounds is not documented: see openEndpoint);
//   out:<endpoint id>  a render endpoint's LOOPBACK (shared mode only; event-driven since Windows 10 1703);
//   app:<exe>          one application's sound: its ROOT process (TrCaptureEnum.h) with its whole tree, through
//                      ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK) -- Windows 10 build 20348+.
//                      LIMIT (fix round RVA-5): ONE root per strip. Two separate processes of one exe (two windows of a
//                      player started apart, two browser profiles) are two roots; the strip captures the OLDEST and says
//                      so in the log -- the other is not heard. Summing two process captures in one lane would need
//                      two clients on two clocks behind one SPSC FIFO: not done.
// A mix format below 16 kHz (a Bluetooth headset's hands-free microphone) is asked at 16 kHz instead, Windows
// converting (AUTOCONVERTPCM | SRC_DEFAULT_QUALITY): the Decimator never sees a rate below its output.
// Loss and return: any failure releases everything and reopens the SAME endpoint id after 0.2 / 1 / 2 / 5 s (5 s
// thereafter), sooner when a device notification kicks it; an application that closed is looked for again by name
// every 2 s. A failing source never stops the others: its lanes carry zeros and INPUT says why. The ladder is reset
// only by a stream that stayed healthy, and a FLAPPING device keeps its failure state until it does (RetryPolicy).
// Written from Microsoft's documentation only (docs/LIVE_PLAN.md section 4.2, the clean room; the pages are listed in
// docs/PROGRESS_L1b.md).
// =============================================================================
#pragma once

#include "TrCaptureCore.h"

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

namespace pbt {
namespace cap {

class WasapiSource : public Source, public std::enable_shared_from_this<WasapiSource> {
public:
    enum class Mode { Input, OutputLoopback, AppLoopback };

    // hidden = process trees an "app:" key never resolves into (SuVtitle.exe's, this engine's).
    WasapiSource(const std::string& key, Mode mode, std::shared_ptr<EventGate> gate, std::vector<uint32_t> hidden);
    ~WasapiSource() override;

    void begin() override;
    bool waitFirstOpen(DWORD ms) override;
    void requestStop() override;
    bool join(DWORD ms) override;
    void kick() override;

private:
    struct Outcome {
        bool        ok = false;
        bool        stopped = false;     // stop() came while opening: no state to report
        bool        permanent = false;   // unsupported: never retried
        std::string state, why;          // when !ok: why = the user's sentence
        std::string detail;              // when !ok: the stage and HRESULT, for the log only
        DWORD       retryMs = 0;         // 0 = the back-off table
    };

    void    threadMain();
    Outcome openEndpoint();
    Outcome openApp();
    Outcome startStream(const char* what);
    Outcome runStream();
    void    closeStream();
    Outcome failure(HRESULT hr, const char* stage);
    void    optOutOfDucking(IMMDevice* dev);   // in: -- on the session the stream will join, before it exists
    HRESULT activateProcessLoopback(uint32_t pid, bool& stopped);
    bool    waitRetry(DWORD ms);         // false = stop requested
    void    log(const std::string& s) { gate_->log("capture: " + key_ + ": " + s); }

    const Mode                 mode_;
    std::shared_ptr<EventGate> gate_;
    StateReporter              reporter_;
    const std::vector<uint32_t> hidden_;
    std::wstring               target_;       // the endpoint id, or the exe name (lower case)

    HANDLE stopEv_ = nullptr, kickEv_ = nullptr, firstEv_ = nullptr, audioEv_ = nullptr;
    BoundedThread thread_;

    // the device thread's own
    IMMDeviceEnumerator* en_ = nullptr;
    IAudioClient*        client_ = nullptr;
    IAudioCaptureClient* capture_ = nullptr;
    HANDLE               proc_ = nullptr;     // app: the target root process (its exit = the source is gone)
    uint32_t             procPid_ = 0;
    std::wstring         procExe_;
    SampleFormat         fmt_;
    std::string          name_;               // the endpoint's friendly name, for the log
    std::vector<float>   conv_;
    RetryPolicy          retry_;              // the back-off, the held INPUT, the log rate limit (fix round RVA-8)
    std::string          openLine_;           // the "capture: opened ..." line of the stream that runs
    GUID                 sessionGuid_ = {};   // in: -- the audio session our stream joins (its ducking opt-out)
    bool                 duckDone_ = false;   // in: -- the session manager took the opt-out (else: on the stream)
    bool                 duckLogged_ = false; // in: -- said once in the log
    IAudioSessionControl2* duckCtl_ = nullptr; // in: -- that session, held while the stream runs (closeStream)
    size_t               rootsLogged_ = 0;    // app: -- how many separate roots the log last named (RVA-5)
};

} // namespace cap
} // namespace pbt
