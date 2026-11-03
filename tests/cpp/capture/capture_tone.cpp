// =============================================================================
// SuVtitle tests/cpp/capture/capture_tone.cpp -- wave L1b (2026-10-25): the application the capture smoke captures
// through "one application's sound" (process loopback, engine/TrCaptureWasapi.cpp).
//
//   capture_tone.exe <event name>
//
// Opens the DEFAULT render endpoint in shared mode and plays SILENCE (so it has an audio session the picker can
// list) until the named event is set -- at most 15 s -- then ONE 440 Hz tone at -60 dBFS for 1.5 s, fades out, and
// exits. Hard caps, whatever the arguments: amplitude 0.001 (-60 dBFS), 2 s of tone. Console exe, started with
// CREATE_NO_WINDOW by the smoke's launcher (which exits at once, so this process is its own root: the smoke must
// not find it inside its own process tree). ASCII only.
// =============================================================================
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "ole32.lib")

namespace {

constexpr double kAmp = 0.001;       // -60 dBFS: the plan's ceiling
constexpr double kToneSec = 1.5;     // <= 2 s: the plan's ceiling
constexpr double kFadeSec = 0.02;
constexpr DWORD  kWaitGoMs = 15000;

template <class T> void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    if (FAILED(::CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioClient* ac = nullptr;
    IAudioRenderClient* rc = nullptr;
    WAVEFORMATEX* wf = nullptr;
    HANDLE ev = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    int rcode = 0;
    bool isFloat = false;
    UINT32 bufFrames = 0;
    if (FAILED(::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
        FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&ac))) ||
        FAILED(ac->GetMixFormat(&wf))) {
        rcode = 4;
    }
    if (!rcode) {
        if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && wf->wBitsPerSample == 32) isFloat = true;
        else if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->wBitsPerSample == 32 &&
                 reinterpret_cast<WAVEFORMATEXTENSIBLE*>(wf)->SubFormat == kSubtypeFloat) isFloat = true;
        else if (wf->wBitsPerSample != 16) rcode = 5;            // only float32 and PCM16 are written here
    }
    if (!rcode && (FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, wf, nullptr)) ||
                   FAILED(ac->GetBufferSize(&bufFrames)) || FAILED(ac->SetEventHandle(ev)) ||
                   FAILED(ac->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&rc))))) {
        rcode = 6;
    }
    if (!rcode) {
        BYTE* p = nullptr;
        if (SUCCEEDED(rc->GetBuffer(bufFrames, &p))) rc->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(ac->Start())) rcode = 7;
    }
    HANDLE go = nullptr;
    for (int i = 0; i < 100 && !go && !rcode; ++i) {
        go = ::OpenEventW(SYNCHRONIZE, FALSE, argv[1]);
        if (!go) ::Sleep(20);
    }
    if (!rcode && !go) rcode = 8;

    if (!rcode) {
        const uint32_t rate = wf->nSamplesPerSec, ch = wf->nChannels;
        const double step = 2.0 * 3.14159265358979323846 * 440.0 / (double)rate;
        const uint64_t toneFrames = (uint64_t)(kToneSec * rate), fade = (uint64_t)(kFadeSec * rate);
        bool playing = false, done = false;
        uint64_t n = 0, tail = 0;
        const ULONGLONG t0 = ::GetTickCount64();
        while (!done) {
            ::WaitForSingleObject(ev, 50);
            if (!playing) {
                if (::WaitForSingleObject(go, 0) == WAIT_OBJECT_0) playing = true;
                else if (::GetTickCount64() - t0 > kWaitGoMs) break;    // never asked: silence only, then out
            }
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) { rcode = 9; break; }
            const UINT32 avail = bufFrames - pad;
            if (!avail) continue;
            BYTE* p = nullptr;
            if (FAILED(rc->GetBuffer(avail, &p))) { rcode = 10; break; }
            for (UINT32 f = 0; f < avail; ++f) {
                double v = 0.0;
                if (playing && n < toneFrames) {
                    double g = 1.0;
                    if (n < fade) g = (double)n / (double)fade;
                    if (toneFrames - n < fade) g = (double)(toneFrames - n) / (double)fade;
                    v = kAmp * g * std::sin(step * (double)n);
                    ++n;
                } else if (playing) {
                    ++tail;
                }
                for (uint32_t c = 0; c < ch; ++c) {
                    if (isFloat) {
                        const float s = (float)v;
                        std::memcpy(p + ((size_t)f * ch + c) * 4, &s, 4);
                    } else {
                        const short s = (short)std::lround(v * 32767.0);
                        std::memcpy(p + ((size_t)f * ch + c) * 2, &s, 2);
                    }
                }
            }
            rc->ReleaseBuffer(avail, 0);
            if (playing && tail > rate / 5) done = true;            // 0.2 s of silence after the tone, then out
        }
        ac->Stop();
    }
    if (go) ::CloseHandle(go);
    if (wf) ::CoTaskMemFree(wf);
    rel(rc);
    rel(ac);
    rel(dev);
    rel(en);
    if (ev) ::CloseHandle(ev);
    ::CoUninitialize();
    return rcode;
}
