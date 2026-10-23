// =============================================================================
// SuVtitle - engine/TrMedia.cpp   (FILE MODE, wave WB -- see TrMedia.h)
//
// SUVTITLE SPLIT (2026-10-23): ported VERBATIM from AudioPB's PatchBus repo, translator/TrMedia.cpp at commit
// 641f1aa (APB_TRANSLATOR "v1.0.6"), for SuVtitleEngine.exe. Only this header and the comments' non-ASCII
// characters changed (house rule: ASCII-only C++).
// =============================================================================
#include "TrMedia.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wmcodecdsp.h>        // CLSID_CResamplerMediaObject: the Resampler DSP, for AIFF

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

// /!\ NO #pragma comment(lib) HERE (WB, after the review): the libraries are named in
// build/CMakeLists.txt, which ALSO delay-loads mfplat.dll and mfreadwrite.dll. Windows N / KN
// ship without them (no Media Feature Pack), and a load-time import would stop the helper from
// starting at all -- the LIVE translator with it. mediaFoundationPresent() below is what every
// Media Foundation call is gated behind.

namespace pbt {

namespace {

// The DLL the presence check looks for; the selftest renames it to prove the refusal.
std::wstring g_mfDllForTest;

constexpr uint32_t kRate16k = 16000;
// A gap in a stream's timestamps smaller than this is jitter (measured: < 12 ms in every
// container tried), not silence; larger ones are filled. A gap larger than kMaxGapSec is a
// clock reset (a broadcast capture's PCR jump), and filling hours of it would be worse than
// not filling it at all.
constexpr double kGapTolSec = 0.100;
constexpr double kMaxGapSec = 30.0;

// ---- seeking (WD) ------------------------------------------------------------------------------
// How far before the anchor Media Foundation is asked to land: the decoder's own warm-up (AAC overlap,
// an MP3-in-MP4 bit reservoir, Opus pre-roll: all under 0.3 s) with room to spare -- decoded and
// thrown away, ~1-5 ms of CPU.
constexpr double kSeekPrerollSec = 1.0;
// The resampler's warm-up before the target: its filter is under 5 ms (measured: exact to the float
// LSB from 5 ms on), so 20 ms.
constexpr double kSeekWarmSec = 0.020;
// A target this close to the start, or this close ahead of where the reader is, is simply decoded
// to: cheaper than a seek, and exact by construction.
constexpr double kSeekNearStartSec = 2.0;
constexpr double kSeekForwardSec   = 10.0;
// Matroska stamps are whole milliseconds; anything spreading more than this is not a trustworthy
// clock and the file falls back to decoding.
constexpr double kSeekStampSpreadSec = 0.002;

std::string hr8(HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "0x%08lx", (unsigned long)hr);
    return b;
}

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Are Media Foundation's DLLs on this PC at all? Windows N / KN without the Media Feature Pack
// have neither. Loaded from System32 ONLY, and KEPT loaded: the delay-load thunks then bind to
// these very modules. Asked before EVERY Media Foundation call (MfScope::init), so a missing DLL
// is a sentence for the tray and never a delay-load exception.
bool mediaFoundationPresent() {
    const wchar_t* need[] = { g_mfDllForTest.empty() ? L"mfplat.dll" : g_mfDllForTest.c_str(),
                              L"mfreadwrite.dll" };
    for (const wchar_t* dll : need)
        if (!::LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
    return true;
}

// COM (MTA) + Media Foundation, for the lifetime of one reader, on the thread that owns it.
struct MfScope {
    bool com = false, mf = false;
    bool init(std::string& err) {
        if (!mediaFoundationPresent()) {
            err = "Windows Media features are not installed on this PC (the Media Feature Pack of "
                  "Windows N / KN editions). Install it from Settings > Apps > Optional features, "
                  "then try again.";
            return false;
        }
        const HRESULT c = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(c);                 // S_OK or S_FALSE: ours to balance. RPC_E_CHANGED_MODE:
                                            // the thread is STA already, which MF also accepts.
        const HRESULT m = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
        mf = SUCCEEDED(m);
        if (!mf) {
            err = "Windows Media Foundation is not available on this system (" + hr8(m) + ").";
            return false;
        }
        return true;
    }
    ~MfScope() {
        if (mf)  ::MFShutdown();
        if (com) ::CoUninitialize();
    }
};

// ---- big-endian readers -------------------------------------------------------------------
inline uint16_t be16(const unsigned char* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
inline uint64_t be64(const unsigned char* p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

bool readAt(HANDLE f, uint64_t off, void* buf, DWORD n) {
    LARGE_INTEGER li; li.QuadPart = (LONGLONG)off;
    if (!::SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ::ReadFile(f, buf, n, &got, nullptr) && got == n;
}

HANDLE openShared(const std::wstring& path) {
    // Shared for read/write/delete: the WebView is playing this very file, and the user may
    // rename it while we read. We never write it.
    return ::CreateFileW(path.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

// ---- 80-bit IEEE extended (AIFF's sample rate) --------------------------------------------
double ext80(const unsigned char* p) {
    const int      e = ((p[0] & 0x7F) << 8) | p[1];
    const uint64_t m = be64(p + 2);
    if (e == 0 && m == 0) return 0.0;
    const double v = std::ldexp((double)m, e - 16383 - 63);
    return (p[0] & 0x80) ? -v : v;
}

// ---- multichannel -> stereo (WB, after the review) ------------------------------------------
// A PROPER fold, by speaker position: the own side at 1.0, the centre and the surrounds at
// 0.707 (-3 dB) into both / their side, the LFE dropped -- normalised so a full-scale signal
// on every channel cannot clip. `mask` = WAVEFORMATEXTENSIBLE speaker bits; 0 (or a mask whose
// channel count disagrees) = the WAVE default order for that count (AIFF has no mask: the same
// order is assumed -- L R C LFE Ls Rs for six). Before this, channels were averaged by
// interleave parity: a 5.1 file put the centre in the LEFT ear only and the LFE in the right.
uint32_t defaultChannelMask(uint32_t ch) {
    switch (ch) {
        case 1: return 0x4;      // FC
        case 2: return 0x3;      // FL FR
        case 3: return 0x7;      // FL FR FC
        case 4: return 0x33;     // FL FR BL BR
        case 5: return 0x37;     // FL FR FC BL BR
        case 6: return 0x3F;     // FL FR FC LFE BL BR
        case 7: return 0x70F;    // FL FR FC LFE BC SL SR
        case 8: return 0x63F;    // FL FR FC LFE BL BR SL SR
        default: return 0;
    }
}

struct Fold { std::vector<float> l, r; };     // per input channel: its weight into L and into R

Fold makeFold(uint32_t inCh, uint32_t mask) {
    Fold f;
    f.l.assign(inCh, 0.0f);
    f.r.assign(inCh, 0.0f);
    uint32_t bits = 0;
    for (uint32_t m = mask; m; m &= m - 1) ++bits;
    if (bits != inCh) mask = defaultChannelMask(inCh);
    const float k3 = 0.70710678f;
    if (!mask) {                                  // > 8 channels and no mask: by parity, -3 dB
        for (uint32_t c = 0; c < inCh; ++c) (c % 2 ? f.r : f.l)[c] = k3;
    } else {
        uint32_t c = 0;
        for (uint32_t bit = 0; bit < 32 && c < inCh; ++bit) {
            if (!(mask & (1u << bit))) continue;
            switch (1u << bit) {
                case 0x1:     f.l[c] = 1.0f; break;                    // front left
                case 0x2:     f.r[c] = 1.0f; break;                    // front right
                case 0x4:     f.l[c] = f.r[c] = k3; break;             // front centre
                case 0x8:     break;                                   // LFE: dropped
                case 0x10: case 0x40: case 0x200: case 0x1000: case 0x8000:
                              f.l[c] = k3; break;                      // left surrounds / heights
                case 0x20: case 0x80: case 0x400: case 0x4000: case 0x20000:
                              f.r[c] = k3; break;                      // right surrounds / heights
                default:      f.l[c] = f.r[c] = 0.5f; break;           // back / top centres
            }
            ++c;
        }
    }
    double sl = 0.0, sr = 0.0;
    for (uint32_t c = 0; c < inCh; ++c) { sl += f.l[c]; sr += f.r[c]; }
    const float g = (float)(1.0 / std::max(1.0, std::max(sl, sr)));
    for (uint32_t c = 0; c < inCh; ++c) { f.l[c] *= g; f.r[c] *= g; }
    return f;
}

// One frame of `inCh` samples -> `outCh` (1 or 2) through the fold.
inline void foldFrame(const Fold& f, const float* in, uint32_t inCh, uint32_t outCh, float* out) {
    double l = 0.0, r = 0.0;
    for (uint32_t c = 0; c < inCh; ++c) { l += f.l[c] * in[c]; r += f.r[c] * in[c]; }
    if (outCh >= 2) { out[0] = (float)l; out[1] = (float)r; }
    else            { out[0] = (float)((l + r) * 0.5); }
}

inline int16_t toPcm16(double v) {
    return (int16_t)std::lround(std::max(-1.0, std::min(1.0, v)) * 32767.0);
}

// The house AV dance for a delete (PB.md): Bitdefender holds a freshly written file for a few
// ms, and a single DeleteFileW in that window leaves a multi-GB .tmp orphan behind.
void deleteWithRetry(const std::wstring& path) {
    for (int t = 0; t < 5; ++t) {
        if (::DeleteFileW(path.c_str()) || ::GetLastError() == ERROR_FILE_NOT_FOUND) return;
        ::Sleep(20);
    }
}

// =============================================================================
// AIFF / AIFC
// =============================================================================
enum class AiffEnc { BeInt, LeInt, BeFloat, U8 };

struct AiffFmt {
    uint16_t ch = 0;
    uint32_t frames = 0;
    uint16_t bits = 0;
    double   rate = 0.0;
    AiffEnc  enc = AiffEnc::BeInt;
    uint32_t bytesPerSample = 0;
    uint64_t dataOff = 0;
    uint64_t dataLen = 0;
    std::string compName;     // for logs
};

bool sniffAiff(HANDLE f) {
    unsigned char h[12];
    if (!readAt(f, 0, h, 12)) return false;
    return std::memcmp(h, "FORM", 4) == 0
        && (std::memcmp(h + 8, "AIFF", 4) == 0 || std::memcmp(h + 8, "AIFC", 4) == 0);
}

// The container, by content, never by name (an .mkv renamed .mp4 is still Matroska). WH: it also decides
// how a chosen sound track is found ("WHICH SOUND TRACK" below).
enum class Container { Other, Wave, Ebml, Iso };
Container sniffContainer(const std::wstring& path) {
    HANDLE f = openShared(path);
    if (f == INVALID_HANDLE_VALUE) return Container::Other;
    unsigned char h[400] = {};
    DWORD got = 0;
    ::ReadFile(f, h, sizeof(h), &got, nullptr);
    ::CloseHandle(f);
    if (got >= 12 && (!std::memcmp(h, "RIFF", 4) || !std::memcmp(h, "RF64", 4) || !std::memcmp(h, "BW64", 4))
        && !std::memcmp(h + 8, "WAVE", 4))
        return Container::Wave;
    if (got >= 4 && h[0] == 0x1A && h[1] == 0x45 && h[2] == 0xDF && h[3] == 0xA3) return Container::Ebml;
    if (got >= 8) {
        static const char* kFirst[] = { "ftyp", "moov", "mdat", "free", "skip", "wide", "pnot", "uuid" };
        for (const char* k : kFirst) if (!std::memcmp(h + 4, k, 4)) return Container::Iso;
    }
    return Container::Other;
}

// WD: is this a container whose Media Foundation SEEK was measured exact (TrMedia.h)? RIFF/RF64/BW64
// WAVE (an AVI is RIFF too, and is NOT trusted), ISO-BMFF (its first box one of theirs), EBML
// (Matroska / WebM). NOT MPEG-TS / M2TS (review P2, TrMedia.h): a capture's lost packets and PTS
// resets put its stamps off the count the linear decode keeps.
bool seekTrustedContainer(Container c) { return c != Container::Other; }

uint32_t gcdU32(uint32_t a, uint32_t b) { while (b) { const uint32_t t = a % b; a = b; b = t; } return a; }

bool parseAiff(HANDLE f, AiffFmt& a, std::string& err) {
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(f, &sz);
    const uint64_t fileLen = (uint64_t)sz.QuadPart;
    unsigned char h[12];
    if (!readAt(f, 0, h, 12)) { err = "This file could not be read."; return false; }
    const bool aifc = std::memcmp(h + 8, "AIFC", 4) == 0;
    bool haveComm = false, haveSsnd = false;
    char comp[5] = "NONE";
    uint64_t at = 12;
    while (at + 8 <= fileLen && !(haveComm && haveSsnd)) {
        unsigned char ck[8];
        if (!readAt(f, at, ck, 8)) break;
        const uint32_t len = be32(ck + 4);
        const uint64_t body = at + 8;
        if (std::memcmp(ck, "COMM", 4) == 0 && len >= 18) {
            unsigned char c[64] = {};
            const DWORD n = (DWORD)std::min<uint32_t>(len, sizeof(c));
            if (!readAt(f, body, c, n)) break;
            a.ch     = be16(c);
            a.frames = be32(c + 2);
            a.bits   = be16(c + 6);
            a.rate   = ext80(c + 8);
            if (aifc && n >= 22) { std::memcpy(comp, c + 18, 4); comp[4] = 0; }
            haveComm = true;
        } else if (std::memcmp(ck, "SSND", 4) == 0 && len >= 8) {
            unsigned char s[8];
            if (!readAt(f, body, s, 8)) break;
            const uint32_t offset = be32(s);
            a.dataOff = body + 8 + offset;
            a.dataLen = (len >= 8 + offset) ? (uint64_t)len - 8 - offset : 0;
            if (a.dataOff + a.dataLen > fileLen) a.dataLen = fileLen > a.dataOff ? fileLen - a.dataOff : 0;
            haveSsnd = true;
        }
        at = body + len + (len & 1);       // chunks are padded to an even length
    }
    if (!haveComm || !haveSsnd || !a.ch || a.rate < 1000.0 || a.rate > 768000.0) {
        err = "This AIFF file is damaged (no usable COMM/SSND chunk).";
        return false;
    }
    // A damaged COMM must not become a giant allocation (WB, after the review): 65535 channels
    // of 64-bit samples at 768 kHz is 400 GB per second of audio.
    if (a.ch > 64 || a.bits == 0 || a.bits > 64) {
        err = "This AIFF file is damaged (an implausible format: " + std::to_string(a.ch) + " channel(s), "
            + std::to_string(a.bits) + "-bit).";
        return false;
    }
    // The compression types, by the four-character code (case matters in AIFC: fl32 vs FL32
    // are both seen in the wild and mean the same thing).
    auto is = [&](const char* k) { return std::memcmp(comp, k, 4) == 0; };
    if (!aifc || is("NONE") || is("twos")) {
        a.enc = AiffEnc::BeInt;                                       // 8-bit AIFF is SIGNED too
        a.bytesPerSample = (uint32_t)((a.bits + 7) / 8);
        a.compName = aifc ? std::string(comp) : "NONE";
    } else if (is("sowt")) {
        a.enc = AiffEnc::LeInt;  a.bytesPerSample = (uint32_t)((a.bits + 7) / 8); a.compName = "sowt";
    } else if (is("in24")) {
        a.enc = AiffEnc::BeInt;  a.bits = 24; a.bytesPerSample = 3; a.compName = "in24";
    } else if (is("in32")) {
        a.enc = AiffEnc::BeInt;  a.bits = 32; a.bytesPerSample = 4; a.compName = "in32";
    } else if (is("fl32") || is("FL32")) {
        a.enc = AiffEnc::BeFloat; a.bits = 32; a.bytesPerSample = 4; a.compName = "fl32";
    } else if (is("fl64") || is("FL64")) {
        a.enc = AiffEnc::BeFloat; a.bits = 64; a.bytesPerSample = 8; a.compName = "fl64";
    } else if (is("raw ")) {
        a.enc = AiffEnc::U8;     a.bits = 8;  a.bytesPerSample = 1; a.compName = "raw";
    } else {
        err = std::string("This AIFF is compressed (") + comp + "), which cannot be read here. "
              "Convert it to WAV.";
        return false;
    }
    if (a.bytesPerSample < 1 || a.bytesPerSample > 8 || (a.enc != AiffEnc::BeFloat && a.bits > 32)) {
        err = "This AIFF uses a sample format that cannot be read here. Convert it to WAV.";
        return false;
    }
    // Trust the data length over the header's frame count when they disagree (a truncated
    // recording keeps its header): never read past what is there.
    const uint64_t frameBytes = (uint64_t)a.ch * a.bytesPerSample;
    const uint64_t have = a.dataLen / frameBytes;
    if (have < a.frames || a.frames == 0) a.frames = (uint32_t)std::min<uint64_t>(have, 0xFFFFFFFFull);
    return true;
}

// One sample -> float in [-1, 1). The divisors are powers of two so a 16-bit source gives
// EXACTLY the float Media Foundation gives for the same PCM (x / 32768) -- which is what makes
// an AIFF decode bit-identical to the WAV it was made from.
inline float aiffSample(const unsigned char* p, const AiffFmt& a) {
    switch (a.enc) {
        case AiffEnc::U8: return ((float)p[0] - 128.0f) / 128.0f;
        case AiffEnc::BeFloat:
            if (a.bits == 32) { uint32_t u = be32(p); float v; std::memcpy(&v, &u, 4); return v; }
            else              { uint64_t u = be64(p); double v; std::memcpy(&v, &u, 8); return (float)v; }
        case AiffEnc::LeInt:
        case AiffEnc::BeInt: {
            const uint32_t n = a.bytesPerSample;
            int64_t v = 0;
            if (a.enc == AiffEnc::BeInt) for (uint32_t k = 0; k < n; ++k) v = (v << 8) | p[k];
            else                         for (uint32_t k = 0; k < n; ++k) v |= (int64_t)p[k] << (8 * k);
            const int shift = (int)(64 - 8 * n);
            v = (v << shift) >> shift;                       // sign-extend
            // Container bits beyond the declared sample size carry nothing (left-justified).
            switch (n) {
                case 1: return (float)v / 128.0f;
                case 2: return (float)v / 32768.0f;
                case 3: return (float)v / 8388608.0f;
                default: return (float)((double)v / 2147483648.0);
            }
        }
    }
    return 0.0f;
}

// =============================================================================
// The Resampler DSP as a bare MFT: float mono in at `inRate`, float mono 16 kHz out. The SAME
// DSP the Source Reader inserts, measured bit-identical to it (TrMedia.h).
// =============================================================================
class Resampler {
public:
    ~Resampler() { release(t_); }
    bool init(uint32_t inRate, std::string& err) {
        HRESULT hr = ::CoCreateInstance(CLSID_CResamplerMediaObject, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&t_));
        if (FAILED(hr)) { err = "The Windows audio resampler is not available (" + hr8(hr) + ")."; return false; }
        IMFMediaType* in = mk(inRate);
        IMFMediaType* out = mk(kRate16k);
        hr = t_->SetInputType(0, in, 0);
        if (SUCCEEDED(hr)) hr = t_->SetOutputType(0, out, 0);
        release(in); release(out);
        if (FAILED(hr)) { err = "The audio resampler refused the format (" + hr8(hr) + ")."; return false; }
        t_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        inRate_ = inRate;
        return true;
    }
    bool push(const float* x, size_t n, std::vector<float>& out) {
        size_t at = 0;
        while (at < n) {
            const size_t k = std::min<size_t>(n - at, 8192);
            IMFSample* s = nullptr; IMFMediaBuffer* b = nullptr;
            if (FAILED(::MFCreateSample(&s)) || FAILED(::MFCreateMemoryBuffer((DWORD)(k * 4), &b))) {
                release(s); release(b); return false;
            }
            BYTE* p = nullptr;
            b->Lock(&p, nullptr, nullptr);
            std::memcpy(p, x + at, k * 4);
            b->Unlock();
            b->SetCurrentLength((DWORD)(k * 4));
            s->AddBuffer(b);
            s->SetSampleTime((LONGLONG)((double)pushed_ * 1e7 / inRate_));
            s->SetSampleDuration((LONGLONG)((double)k * 1e7 / inRate_));
            const HRESULT hr = t_->ProcessInput(0, s, 0);
            release(b); release(s);
            if (hr == MF_E_NOTACCEPTING) { if (!drain(out)) return false; continue; }
            if (FAILED(hr)) return false;
            pushed_ += k;
            at += k;
            if (!drain(out)) return false;
        }
        return true;
    }
    bool finish(std::vector<float>& out) {
        t_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        return drain(out);
    }
private:
    static IMFMediaType* mk(uint32_t rate) {
        IMFMediaType* m = nullptr;
        ::MFCreateMediaType(&m);
        m->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        m->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
        m->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        m->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 1);
        m->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 32);
        m->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        m->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * 4);
        m->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
        return m;
    }
    bool drain(std::vector<float>& out) {
        for (;;) {
            IMFSample* s = nullptr; IMFMediaBuffer* b = nullptr;
            if (FAILED(::MFCreateSample(&s)) || FAILED(::MFCreateMemoryBuffer(65536, &b))) {
                release(s); release(b); return false;
            }
            s->AddBuffer(b);
            MFT_OUTPUT_DATA_BUFFER od{};
            od.pSample = s;
            DWORD st = 0;
            const HRESULT hr = t_->ProcessOutput(0, 1, &od, &st);
            if (SUCCEEDED(hr)) {
                BYTE* p = nullptr; DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                    out.insert(out.end(), (const float*)p, (const float*)p + len / 4);
                    b->Unlock();
                }
            }
            if (od.pEvents) od.pEvents->Release();
            release(b); release(s);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return true;
            if (FAILED(hr)) return false;
        }
    }
    IMFTransform* t_ = nullptr;
    uint32_t      inRate_ = 0;
    uint64_t      pushed_ = 0;
};

// =============================================================================
// ISO-BMFF / QuickTime edit lists
// =============================================================================
struct BoxHdr { uint64_t size = 0; char type[5] = {}; uint32_t hdr = 0; };

bool boxAt(const unsigned char* p, size_t avail, BoxHdr& b) {
    if (avail < 8) return false;
    b.size = be32(p);
    std::memcpy(b.type, p + 4, 4); b.type[4] = 0;
    b.hdr = 8;
    if (b.size == 1) { if (avail < 16) return false; b.size = be64(p + 8); b.hdr = 16; }
    else if (b.size == 0) b.size = avail;
    return b.size >= b.hdr && b.size <= avail;
}

// The interesting bits of one 'trak'. WH: + its track_ID and its sample description, the two facts that
// tie it to the stream Media Foundation makes of it ("WHICH SOUND TRACK" below).
struct TrakFacts {
    bool     sound = false;
    uint32_t mdhdScale = 0;
    std::vector<std::pair<int64_t, uint64_t>> edits;
    uint32_t trackId = 0;                    // tkhd track_ID
    std::vector<unsigned char> stsd;         // the WHOLE 'stsd' box, header included
    char     entry[5] = {};                  // its first sample entry's type ("mp4a", "ac-3", "sowt"...)
};

// `parent` = the box these boxes sit in: a handler is the TRACK's only directly under 'mdia' (a QuickTime
// 'minf' holds a DATA handler too -- 'dhlr'/'alis' -- which must not unset `sound`).
void walk(const unsigned char* p, size_t n, TrakFacts& t, int depth, const char* parent = "trak") {
    size_t at = 0;
    while (at + 8 <= n && depth < 8) {
        BoxHdr b;
        if (!boxAt(p + at, n - at, b)) return;
        const unsigned char* body = p + at + b.hdr;
        const size_t blen = (size_t)(b.size - b.hdr);
        if (!std::strcmp(b.type, "mdia") || !std::strcmp(b.type, "edts") || !std::strcmp(b.type, "minf")
            || !std::strcmp(b.type, "stbl")) {
            walk(body, blen, t, depth + 1, b.type);
        } else if (!std::strcmp(b.type, "tkhd") && !std::strcmp(parent, "trak") && blen >= 24) {
            t.trackId = body[0] == 1 ? be32(body + 20) : be32(body + 12);
        } else if (!std::strcmp(b.type, "stsd") && !std::strcmp(parent, "stbl") && blen >= 16) {
            // capped like everything read from a moov: a sample description is a few hundred bytes
            if (b.size <= (1u << 20)) t.stsd.assign(p + at, p + at + (size_t)b.size);
            std::memcpy(t.entry, body + 12, 4);
            for (int k = 0; k < 4; ++k) if (t.entry[k] < 32 || t.entry[k] > 126) t.entry[k] = '?';
        } else if (!std::strcmp(b.type, "hdlr") && !std::strcmp(parent, "mdia") && blen >= 12) {
            t.sound = std::memcmp(body + 8, "soun", 4) == 0;
        } else if (!std::strcmp(b.type, "mdhd") && blen >= 24) {
            t.mdhdScale = body[0] == 1 ? be32(body + 20) : be32(body + 12);
        } else if (!std::strcmp(b.type, "elst") && blen >= 8) {
            const bool v1 = body[0] == 1;
            const uint32_t cnt = be32(body + 4);
            const size_t es = v1 ? 20 : 12;
            for (uint32_t i = 0; i < cnt && 8 + (size_t)(i + 1) * es <= blen; ++i) {
                const unsigned char* e = body + 8 + i * es;
                const uint64_t dur = v1 ? be64(e) : be32(e);
                const int64_t  mt  = v1 ? (int64_t)be64(e + 8) : (int64_t)(int32_t)be32(e + 4);
                t.edits.emplace_back(mt, dur);
            }
        }
        at += (size_t)b.size;
    }
}

} // namespace

bool isAiffFile(const std::wstring& path) {
    HANDLE f = openShared(path);
    if (f == INVALID_HANDLE_VALUE) return false;
    const bool r = sniffAiff(f);
    ::CloseHandle(f);
    return r;
}

// ISO-BMFF / QuickTime: EVERY trak of the moov, in moov order, and the movie's timescale. false = not one of
// those files, no moov, a moov over 64 MB, or a read error. (WH: was the first sound trak's edit list only.)
static bool isoBmffTraks(const std::wstring& path, std::vector<TrakFacts>& traks, uint32_t& mvhdScale) {
    traks.clear();
    mvhdScale = 0;
    HANDLE f = openShared(path);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(f, &sz);
    const uint64_t len = (uint64_t)sz.QuadPart;
    uint64_t at = 0, moovAt = 0, moovLen = 0;
    bool first = true, ok = false;
    // Top level only, by header: an mdat of gigabytes is skipped, not read.
    while (at + 8 <= len) {
        unsigned char h[16];
        const DWORD want = (DWORD)std::min<uint64_t>(16, len - at);
        if (!readAt(f, at, h, want)) break;
        uint64_t size = be32(h);
        uint32_t hdr = 8;
        if (size == 1 && want >= 16) { size = be64(h + 8); hdr = 16; }
        else if (size == 0) size = len - at;
        char type[5]; std::memcpy(type, h + 4, 4); type[4] = 0;
        if (first) {
            // Not ISO-BMFF / QuickTime unless the first box is one of theirs.
            static const char* kFirst[] = { "ftyp", "moov", "mdat", "free", "skip", "wide", "pnot", "uuid" };
            bool known = false;
            for (const char* k : kFirst) known |= !std::strcmp(type, k);
            if (!known) break;
            first = false;
        }
        // `size > len - at`, never `at + size > len`: a crafted 64-bit size wraps the sum and the
        // walk would run for ever (WB, after the review).
        if (size < hdr || size > len - at) break;
        if (!std::strcmp(type, "moov")) { moovAt = at + hdr; moovLen = size - hdr; ok = true; break; }
        at += size;
    }
    if (!ok || moovLen > (64ull << 20)) { ::CloseHandle(f); return false; }   // 64 MB moov cap
    std::vector<unsigned char> moov((size_t)moovLen);
    const bool got = readAt(f, moovAt, moov.data(), (DWORD)moovLen);
    ::CloseHandle(f);
    if (!got) return false;

    size_t p = 0;
    while (p + 8 <= moov.size() && traks.size() < 256) {
        BoxHdr b;
        if (!boxAt(moov.data() + p, moov.size() - p, b)) break;
        const unsigned char* body = moov.data() + p + b.hdr;
        const size_t blen = (size_t)(b.size - b.hdr);
        if (!std::strcmp(b.type, "mvhd") && blen >= 24) {
            mvhdScale = body[0] == 1 ? be32(body + 20) : be32(body + 12);
        } else if (!std::strcmp(b.type, "trak")) {
            traks.emplace_back();
            walk(body, blen, traks.back(), 0);
        }
        p += (size_t)b.size;
    }
    return true;
}

// One trak's edit list as the reader applies it: lead = its empty edits (movie timescale), trim = the first
// real edit's media_time (its own timescale). true when it moves the audio.
static bool isoEditOf(const TrakFacts& t, uint32_t mvhdScale, double& leadSec, double& trimSec) {
    leadSec = trimSec = 0.0;
    if (!mvhdScale || !t.mdhdScale || t.edits.empty()) return false;
    double lead = 0.0;
    for (const auto& e : t.edits) {
        if (e.first == -1) { lead += (double)e.second / (double)mvhdScale; continue; }
        trimSec = (double)e.first / (double)t.mdhdScale;
        break;
    }
    leadSec = lead;
    return leadSec > 0.0 || trimSec > 0.0;
}

bool isoBmffAudioEdit(const std::wstring& path, double& leadSec, double& trimSec) {
    leadSec = trimSec = 0.0;
    std::vector<TrakFacts> traks;
    uint32_t mvhdScale = 0;
    if (!isoBmffTraks(path, traks, mvhdScale)) return false;
    for (const TrakFacts& t : traks)
        if (t.sound) return isoEditOf(t, mvhdScale, leadSec, trimSec);     // the FIRST sound trak
    return false;
}

static IMFMediaType* audioType(const GUID& sub, uint32_t rate, uint32_t ch, uint32_t bits);

// =============================================================================
// MediaReader
// =============================================================================
struct MediaReader::Impl {
    MfScope   mf;
    MediaInfo info;
    Out       out = Out::Mono16k;
    uint32_t  rate = 0, ch = 0;          // OUTPUT format
    bool      srcEof = false;            // the decoder has nothing more
    uint64_t  pos = 0;                   // frames handed out
    uint64_t  emitted = 0;               // frames placed on the timeline (handed out + pending)
    std::vector<float>   pendF;          // Mono16k, waiting to be handed out
    std::vector<int16_t> pendI;          // Pcm16Native, interleaved

    // ---- Media Foundation --------------------------------------------------------------
    IMFSourceReader* rd = nullptr;
    // WH: THE STREAM DECODED -- every Media Foundation call below names it (before WH, each one said
    // MF_SOURCE_READER_FIRST_AUDIO_STREAM), and `track` is what open() was asked for, so a re-open (a
    // decoded seek going backwards) finds the very same one.
    DWORD      aIdx = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
    AudioTrack track;
    // WHAT MF IS DELIVERING NOW: the type we asked for -- or, after the decoder's own format
    // changed mid-stream (a broadcast .ts switching 2.0 <-> 5.1), whatever MF announced with
    // MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED until we have asked for ours again.
    GUID     curSub = GUID_NULL;         // MFAudioFormat_Float or MFAudioFormat_PCM (16-bit)
    uint32_t curCh = 0, curRate = 0, curMask = 0;
    bool     firstSample = true;
    double   shiftSec = 0.0;             // presentation time = decoded time + shiftSec
    uint64_t trimLeft = 0;               // decoded frames still to drop (edit-list priming)
    // THE LEAD, owed at the front and handed out from a COUNTER: an audio track that starts
    // 40 minutes into a container is not 150 MB of zeros in memory (WB, after the review).
    uint64_t leadZeros = 0;
    Fold     fold;                       // curCh -> ch (built when curCh / curMask change)
    uint32_t foldCh = 0, foldMask = 0;

    // ---- AIFF ------------------------------------------------------------------------------
    HANDLE   f = INVALID_HANDLE_VALUE;
    AiffFmt  af;
    uint64_t aiffDone = 0;               // frames read
    Resampler* rs = nullptr;             // Mono16k only (AIFF, and Media Foundation below 16 kHz != native)
    bool     rsFinished = false;

    // ---- WD: the Mono16k chain over Media Foundation, and seeking ---------------------------------
    // Media Foundation delivers float MONO at the NATIVE rate; OUR resampler -- the same DSP, measured
    // bit-identical to Media Foundation's own float/16k/mono -- takes it to 16 kHz, anchored on the
    // stream's decoded sample 0, and place() puts it on the timeline exactly as before (TrMedia.h).
    // Owning the resampler is what lets a seek restart it ON THE SAME GRID.
    static constexpr int64_t kNoC16 = std::numeric_limits<int64_t>::min();
    std::wstring path;                   // for a re-open (a decoded seek going backwards)
    bool     mfMono16 = false;           // this reader runs that chain
    uint32_t natRate = 0;                // what Media Foundation delivers (the source's rate)
    double   firstTs = 0.0;              // the decoded stamp of the stream's sample 0 ...
    bool     haveFirstTs = false;        // ... once known (a NEGATIVE first stamp is a stamp: AAC priming)
    uint64_t natCount = 0;               // native samples of the decoded stream so far (gap zeros incl.)
    uint64_t rsK = 0;                    // 16 kHz outputs of the decoded stream so far
    int64_t  c16 = kNoC16;               // the timeline frame of output 0 (lead - trim), once placed
    bool     seekTrusted = false;        // the container's seek stamps were measured exact
    bool     demoted = false;            // ... but THIS file's stamps were not: decode instead
    const char* seekHow = "none";
    std::vector<float> scratch;          // one decoded buffer, reused
    // THE STAMPS' OWN BIAS against the decoded count, measured on the stream's first seconds (the
    // linear decode places by COUNT and ignores sub-100 ms stamp jitter). 0 where stamps are exact;
    // Matroska's are whole ms and carry the AAC priming rounded (measured -10 samples at 44.1 kHz):
    // without subtracting it a seek landed 4 samples (16 kHz) off.
    std::vector<std::pair<double, uint64_t>> startMarks;   // (stamp, natCount) of the first buffers
    static constexpr double kBiasSpanSec = 3.0;
    bool     seekedOnce = false;         // startMarks describe the stream from its start only
    // C3 (review): the first seconds are learnt until a buffer is SEEN that far -- recorded or not. A
    // stamp jump across front + 2.9..3.0 s (an audio gap) or a buffer grid that never lands there
    // recorded nothing past 2.9 s, and the loop waiting for one decoded the WHOLE file into memory.
    bool     marksSpan = false;
    // P2 (review): the stamps' offset from the count, as the second buffer set it (the first may be
    // partial) -- a buffer that leaves it by more than kSeekStampSpreadSec without being a gap the
    // linear decode fills (a lost 21 ms frame, a reset, a 30 s jump) DEMOTES the file.
    static constexpr int64_t kNoOff = std::numeric_limits<int64_t>::min();
    int64_t  offRef = kNoOff;
    // P3 (review): THE SEEK'S ABORT -- the caller's predicate, polled between decoded buffers. An
    // aborted seek leaves the reader BROKEN (half moved): the next seek re-opens it, a read refuses.
    const std::function<bool()>* abortFn = nullptr;
    bool     inSeek = false, aborted = false, broken = false;
    uint64_t seekDecoded = 0;            // native frames the current/last seek decoded

    bool pollAbort() {
        if (abortFn && *abortFn && (*abortFn)()) { aborted = true; broken = true; return true; }
        return false;
    }

    ~Impl() { closeAll(); }
    void closeAll() {
        release(rd);
        if (f != INVALID_HANDLE_VALUE) { ::CloseHandle(f); f = INVALID_HANDLE_VALUE; }
        delete rs; rs = nullptr;
    }

    const Fold& foldFor(uint32_t inCh, uint32_t mask) {
        if (inCh != foldCh || mask != foldMask) { fold = makeFold(inCh, mask); foldCh = inCh; foldMask = mask; }
        return fold;
    }

    // Place `n` decoded frames (already in the OUTPUT format) that START at decoded time `tsSec`
    // on the media timeline: drop edit-list priming, fill a late start or a gap with silence.
    // `outIndex` (WD) = the index of data[0] in the decoded OUTPUT stream: the first placement records
    // where output 0 sits on the timeline (c16), which is all a seek needs to land on the same frame.
    template <class T>
    void place(std::vector<T>& pend, const T* data, size_t n, double tsSec, uint64_t outIndex = 0) {
        size_t skip = 0;
        if (trimLeft) { skip = (size_t)std::min<uint64_t>(trimLeft, n); trimLeft -= skip; }
        if (skip >= n) return;
        const double presSec = tsSec + shiftSec + (double)skip / (double)rate;
        const int64_t target = (int64_t)std::llround(presSec * (double)rate);
        const int64_t delta  = target - (int64_t)emitted;
        const int64_t tol    = (int64_t)(kGapTolSec * rate);
        const int64_t cap    = (int64_t)(kMaxGapSec * rate);
        if (firstSample) {
            // The FIRST sample's time is the lead, padded exactly (no tolerance): frame 0 must be
            // media time 0 for the cues and the proxy alike. NOT capped at kMaxGapSec (that cap is
            // for clock resets mid-stream) -- only by the container's own duration.
            const int64_t firstCap = info.durationMs
                ? (int64_t)(((double)info.durationMs / 1000.0 + 1.0) * rate)
                : std::numeric_limits<int64_t>::max();
            if (delta > 0 && delta <= firstCap) {
                leadZeros += (uint64_t)delta;
                emitted   += (uint64_t)delta;
            }
            firstSample = false;
            c16 = (int64_t)emitted - (int64_t)(outIndex + skip);
        } else if (!mfMono16 && delta > tol && delta <= cap) {
            // (The Mono16k chain made its gaps silence at the native rate already -- nativeIn -- and
            // hands place() a contiguous, count-stamped stream: output k is ALWAYS frame c16 + k,
            // which is the one invariant a seek relies on.)
            pend.insert(pend.end(), (size_t)delta * ch, T{});
            emitted += (uint64_t)delta;
        }
        pend.insert(pend.end(), data + skip * ch, data + n * ch);
        emitted += (uint64_t)(n - skip);
    }

    // One decoded buffer, in MF's CURRENT format (cur*), into the output format, placed. Pcm16Native
    // only (the Mono16k chain is readNative / nativeIn below). false = a format we cannot use at all
    // (a sample rate other than the output's).
    bool consumeMf(const BYTE* p, DWORD len, double tsSec) {
        const bool isFloat = curSub == MFAudioFormat_Float;
        if (!curCh || (!isFloat && curSub != MFAudioFormat_PCM)) return false;
        const size_t bytesPer = isFloat ? 4 : 2;
        const size_t n = len / (bytesPer * curCh);
        auto sampleAt = [&](size_t i, uint32_t c) -> float {
            return isFloat ? ((const float*)p)[i * curCh + c]
                           : (float)((const int16_t*)p)[i * curCh + c] / 32768.0f;
        };
        std::vector<float> fr(curCh);
        if (out == Out::Mono16k) return false;          // never: Mono16k runs readNative
        if (curRate != rate) return false;
        if (!isFloat && curCh == ch) { place(pendI, (const int16_t*)p, n, tsSec); return true; }
        const Fold& fo = foldFor(curCh, curMask);
        std::vector<int16_t> pcm(n * ch);
        float o[2];
        for (size_t i = 0; i < n; ++i) {
            for (uint32_t c = 0; c < curCh; ++c) fr[c] = sampleAt(i, c);
            if (curCh == ch) { for (uint32_t c = 0; c < ch; ++c) o[c] = fr[c]; }   // float, same layout
            else foldFrame(fo, fr.data(), curCh, ch, o);
            for (uint32_t c = 0; c < ch; ++c) pcm[i * ch + c] = toPcm16(o[c]);
        }
        place(pendI, pcm.data(), n, tsSec);
        return true;
    }

    // A sample flagged MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED: read the type now in force; true = it
    // is the very one that was (a no-op notice). /!\ WD, 11/10: the AAC decoder flags its FIRST sample so,
    // with exactly the type asked for -- and setting a type again FLUSHES the decoder: 0 to 2 frames were
    // lost at the start, at random (a 16 kHz AAC: its second sample stamped 0.064 or 0.192 s from one
    // open to the next; at 44.1 kHz such a 46 ms loss is under the gap rule and the whole timeline
    // moved). So the type is set again only when it really is another one (readNative, pullMf).
    bool rereadType() {
        const GUID     sub = curSub;
        const uint32_t c = curCh, r = curRate, m = curMask;
        readCurrentType();
        return curSub == sub && curCh == c && curRate == r && curMask == m;
    }
    // ... and a REAL change is consumed as it is whenever it can be -- PCM or float at the rate this
    // reader runs at, any layout (toMono / consumeMf fold it): setting ours again would flush the decoder
    // there too (measured: a 2.0 -> 5.1 TS lost 2-3 AAC frames at random in the FPROXY copy, which then
    // ran 43-64 ms short of the analysis). Only a type nothing here can read is asked for again.
    bool consumable(uint32_t wantRate) const {
        return (curSub == MFAudioFormat_Float || curSub == MFAudioFormat_PCM) && curCh && curRate == wantRate;
    }

    // Read MF's current output type into cur*.
    void readCurrentType() {
        IMFMediaType* t = nullptr;
        if (FAILED(rd->GetCurrentMediaType(aIdx, &t))) return;
        UINT32 c = 0, r = 0, m = 0;
        t->GetGUID(MF_MT_SUBTYPE, &curSub);
        t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &c);
        t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &r);
        if (FAILED(t->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &m))) m = 0;
        curCh = c; curRate = r; curMask = m;
        release(t);
    }

    // Ask MF for OUR output type again (after a mid-stream change): the requested format first,
    // else -- for the proxy -- float at whatever layout the decoder now has, which we fold.
    // (Mono16k asks for MONO at the rate it opened with: our resampler is built for that rate.)
    void restoreOutputType() {
        IMFMediaType* t = (out == Out::Mono16k) ? audioType(MFAudioFormat_Float, natRate, 1, 32)
                                                : audioType(MFAudioFormat_PCM, rate, ch, 16);
        HRESULT hr = rd->SetCurrentMediaType(aIdx, nullptr, t);
        release(t);
        if (FAILED(hr) && out == Out::Pcm16Native) {
            IMFMediaType* nat = nullptr;
            UINT32 nc = 0;
            if (SUCCEEDED(rd->GetNativeMediaType(aIdx, 0, &nat))) {
                nat->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &nc);
                release(nat);
            }
            if (nc) {
                IMFMediaType* fl = audioType(MFAudioFormat_Float, rate, nc, 32);
                rd->SetCurrentMediaType(aIdx, nullptr, fl);
                release(fl);
            }
        }
        readCurrentType();
    }

    // Pull ONE sample from the decoder into the pending buffer. false = error (err set).
    bool pullMf(std::string& err) {
        DWORD si = 0, flags = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
        const HRESULT hr = rd->ReadSample(aIdx, 0, &si, &flags, &ts, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            release(s);
            err = "The file is damaged or truncated at " + mmss(emitted / (double)rate)
                + " (" + hr8(hr) + ").";
            return false;
        }
        // THE DECODER'S FORMAT CHANGED (WB, after the review): this sample is already in the new
        // one -- read it as such -- and the next ones are asked for in ours again.
        const bool changed = (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0;
        bool noOp = false;
        if (changed) noOp = rereadType();
        bool usable = true;
        if (s) {
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                BYTE* p = nullptr; DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                    usable = consumeMf(p, len, (double)ts / 1e7);
                    b->Unlock();
                }
                release(b);
            }
            release(s);
        }
        if (changed) {
            // (never set again a type that can be read as it is: that FLUSHES the decoder -- rereadType)
            if (!noOp && !consumable(rate)) restoreOutputType();
            const bool ok = (curSub == MFAudioFormat_Float || curSub == MFAudioFormat_PCM) && curCh
                         && curRate == (out == Out::Mono16k ? kRate16k : rate);
            if (!ok) {
                err = "The audio format changes at " + mmss(emitted / (double)rate)
                    + " in a way that cannot be decoded here. Convert the file to WAV.";
                return false;
            }
        } else if (!usable) {
            err = "The audio format changes at " + mmss(emitted / (double)rate)
                + " in a way that cannot be decoded here. Convert the file to WAV.";
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) srcEof = true;
        return true;
    }

    // =========================================================================================
    // WD: the Mono16k chain over Media Foundation (see the members) and the seek machinery.
    // =========================================================================================
    // One buffer in MF's CURRENT format -> native-rate MONO floats. false = unusable (a rate other
    // than the one the resampler was built for).
    bool toMono(const BYTE* p, DWORD len, std::vector<float>& mono) {
        mono.clear();
        const bool isFloat = curSub == MFAudioFormat_Float;
        if (!curCh || (!isFloat && curSub != MFAudioFormat_PCM) || curRate != natRate) return false;
        const size_t n = len / ((isFloat ? 4u : 2u) * curCh);
        mono.resize(n);
        if (isFloat && curCh == 1) { if (n) std::memcpy(mono.data(), p, n * 4); return true; }
        const Fold& fo = foldFor(curCh, curMask);
        std::vector<float> fr(curCh);
        for (size_t i = 0; i < n; ++i) {
            for (uint32_t c = 0; c < curCh; ++c)
                fr[c] = isFloat ? ((const float*)p)[i * curCh + c] : (float)((const int16_t*)p)[i * curCh + c] / 32768.0f;
            foldFrame(fo, fr.data(), curCh, 1, &mono[i]);
        }
        return true;
    }

    // ONE Media Foundation sample as native mono floats, with its decoded stamp. false = an error
    // (`err`); an empty `mono` = no sample this time (a stream tick, the end).
    bool readNative(std::vector<float>& mono, double& tsSec, std::string& err) {
        mono.clear();
        DWORD si = 0, flags = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
        const HRESULT hr = rd->ReadSample(aIdx, 0, &si, &flags, &ts, &s);
        if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
            release(s);
            err = "The file is damaged or truncated at " + mmss(emitted / (double)rate)
                + " (" + hr8(hr) + ").";
            return false;
        }
        const bool changed = (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0;
        bool noOp = false;
        if (changed) noOp = rereadType();
        bool usable = true;
        if (s) {
            IMFMediaBuffer* b = nullptr;
            if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
                BYTE* p = nullptr; DWORD len = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &len))) {
                    usable = toMono(p, len, mono);
                    b->Unlock();
                }
                release(b);
            }
            release(s);
            tsSec = (double)ts / 1e7;
        }
        if (changed) {
            // (as pullMf: the sample that announced the change is read in the new format if we can,
            // dropped if we cannot, and the next ones come in ours again -- unless it changed nothing, or
            // the new type can be read as it is: see rereadType / consumable)
            if (!noOp && !consumable(natRate)) restoreOutputType();
            const bool ok = (curSub == MFAudioFormat_Float || curSub == MFAudioFormat_PCM) && curCh && curRate == natRate;
            if (!ok) {
                err = "The audio format changes at " + mmss(emitted / (double)rate)
                    + " in a way that cannot be decoded here. Convert the file to WAV.";
                return false;
            }
            if (!usable) mono.clear();
        } else if (!usable) {
            err = "The audio format changes at " + mmss(emitted / (double)rate)
                + " in a way that cannot be decoded here. Convert the file to WAV.";
            return false;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) srcEof = true;
        if (inSeek) seekDecoded += mono.size();
        return true;
    }

    // Native mono -> 16 kHz (our resampler, or straight through at 16 kHz) -> the timeline. The
    // stamp handed to place() is COUNT-based (output k sits at firstTs + k / 16 kHz): gaps were
    // already made silence at the native rate (nativeIn), so place() only ever sees a contiguous
    // stream -- and does exactly what it did for Media Foundation's own 16 kHz buffers.
    bool toTimeline(const float* x, size_t n) {
        if (!n) return true;
        if (natRate == kRate16k) {
            place(pendF, x, n, firstTs + (double)rsK / kRate16k, rsK);
            rsK += n;
            return true;
        }
        std::vector<float> y;
        if (!rs || !rs->push(x, n, y)) return false;
        if (!y.empty()) {
            place(pendF, y.data(), y.size(), firstTs + (double)rsK / kRate16k, rsK);
            rsK += y.size();
        }
        return true;
    }

    // THE LINEAR DECODE'S STAMP RULE for a buffer stamped `ts` (natCount = the samples before it): a
    // forward gap in the stamps (> 100 ms, <= 30 s) is silence to insert -- returned -- and anything
    // else is ignored, exactly as v1 did. WD, review P2: a trusted container's seek maps stamps to the
    // count with the offset of the stream's first seconds; a buffer that leaves that offset by more
    // than kSeekStampSpreadSec WITHOUT being a gap filled here (a lost frame, a reset, a jump past
    // 30 s) means the two timelines now differ: the file is DEMOTED (its seeks decode). A filled gap
    // realigns the count ON the stamps -- an offset of 0 from there on: where the offset before it was
    // not 0 (Matroska's ms stamps and AAC priming: -10 samples on a 44.1 kHz MKV with a 1 s gap -- the
    // seek's bias, learnt before it, no longer holds after it), the file is demoted too. The offset is
    // then learnt again.
    int64_t stampRule(double ts) {
        if (natCount == 0) return 0;
        const double  expect = firstTs + (double)natCount / natRate;
        const int64_t delta  = (int64_t)std::llround((ts - expect) * (double)natRate);
        const int64_t tol    = (int64_t)(kGapTolSec * natRate), cap = (int64_t)(kMaxGapSec * natRate);
        if (delta > tol && delta <= cap) {
            if (offRef != kNoOff && offRef != 0) demoted = true;
            offRef = kNoOff;
            return delta;
        }
        if (offRef == kNoOff) offRef = delta;
        else if (std::llabs(delta - offRef) > (int64_t)(kSeekStampSpreadSec * natRate)) demoted = true;
        return 0;
    }
    // The stamps of the stream's first seconds (the seek's bias): recorded within kBiasSpanSec of the
    // first, and marksSpan once ANY buffer is seen past front + kBiasSpanSec - 0.1 (C3).
    void noteStartMark(double ts) {
        if (seekedOnce) return;
        if (startMarks.empty() || ts - startMarks.front().first < kBiasSpanSec) startMarks.emplace_back(ts, natCount);
        if (ts - startMarks.front().first >= kBiasSpanSec - 0.1) marksSpan = true;
    }

    // One decoded buffer of the stream, stamped `ts`: a forward gap in the stamps (> 100 ms, < 30 s)
    // becomes silence AT THE NATIVE RATE first, so the resampler hears it as such.
    bool nativeIn(const float* x, size_t n, double ts) {
        if (!n) return true;
        if (!haveFirstTs) { firstTs = ts; haveFirstTs = true; }
        const int64_t delta = stampRule(ts);
        if (delta > 0) {
            std::vector<float> z((size_t)std::min<int64_t>(delta, (int64_t)natRate), 0.0f);
            for (int64_t left = delta; left > 0;) {
                const size_t k = (size_t)std::min<int64_t>(left, (int64_t)z.size());
                if (!toTimeline(z.data(), k)) return false;
                left -= (int64_t)k;
            }
            natCount += (uint64_t)delta;
        }
        noteStartMark(ts);
        if (!toTimeline(x, n)) return false;
        natCount += n;
        return true;
    }

    // The midrange of the offsets the stamps imply against a count: (stamp - firstTs) * rate - count.
    // `from` = the first mark that votes. false = no mark.
    bool stampOffset(const std::vector<std::pair<double, uint64_t>>& m, size_t from, int64_t& lo, int64_t& hi) const {
        lo = std::numeric_limits<int64_t>::max();
        hi = std::numeric_limits<int64_t>::min();
        for (size_t i = from; i < m.size(); ++i) {
            const int64_t o = (int64_t)std::llround((m[i].first - firstTs) * (double)natRate) - (int64_t)m[i].second;
            lo = std::min(lo, o);
            hi = std::max(hi, o);
        }
        return from < m.size();
    }

    void drainResampler() {
        if (!rs || rsFinished) return;
        std::vector<float> y;
        rs->finish(y);
        rsFinished = true;
        if (!y.empty()) {
            place(pendF, y.data(), y.size(), firstTs + (double)rsK / kRate16k, rsK);
            rsK += y.size();
        }
    }

    // Pull ONE Media Foundation sample through the Mono16k chain. false = error (err set).
    bool pullMf16(std::string& err) {
        double ts = 0.0;
        if (!readNative(scratch, ts, err)) return false;
        if (!scratch.empty() && !nativeIn(scratch.data(), scratch.size(), ts)) {
            err = "The audio resampler failed.";
            return false;
        }
        if (srcEof) drainResampler();
        return true;
    }

    bool pull(std::string& err) {
        if (f != INVALID_HANDLE_VALUE) return pullAiff(err);
        return out == Out::Mono16k ? pullMf16(err) : pullMf(err);
    }

    // Hand out -- and throw away -- frames until the position is `target`, decoding as needed.
    // Past the end, it stops at eof().
    bool discardTo(uint64_t target, std::string& err) {
        while (pos < target) {
            if (leadZeros) {
                const uint64_t z = std::min<uint64_t>(leadZeros, target - pos);
                leadZeros -= z;
                pos += z;
                continue;
            }
            if (!pendF.empty()) {
                const size_t k = (size_t)std::min<uint64_t>(pendF.size(), target - pos);
                pendF.erase(pendF.begin(), pendF.begin() + (std::ptrdiff_t)k);
                pos += k;
                continue;
            }
            if (srcEof) return true;
            if (pollAbort()) return false;
            if (!pull(err)) return false;
        }
        return true;
    }

    // DECODED SEEK, FAST (WD): what the linear decode would do, without the part of its work nobody
    // will read. Native samples wholly before the anchor j0 are only COUNTED (the gap rule applied
    // exactly as nativeIn applies it), and the chain restarts at j0 on a period of the ratio -- so the
    // outputs from the target on are the linear ones to the float LSB, at about half the cost (the
    // resampler was half of it: 5.8 s -> 2.9 s to reach 1:20:00 in a 2 h MP3).
    bool fastForward(uint64_t target, std::string& err) {
        while (c16 == kNoC16 && !srcEof) {                   // the timeline's anchor: the first sample placed
            if (pollAbort()) return false;
            if (!pullMf16(err)) return false;
        }
        if (c16 == kNoC16 || srcEof) return discardTo(target, err);
        int64_t pin = 1, pout = 1;
        period(natRate, pin, pout);
        const int64_t warm = natRate == kRate16k ? 0 : (int64_t)(kSeekWarmSec * kRate16k);
        int64_t k0 = (int64_t)target - c16 - warm;
        if (k0 <= (int64_t)rsK + (int64_t)kRate16k) return discardTo(target, err);
        k0 = k0 / pout * pout;
        const int64_t j0 = k0 / pout * pin;
        bool restarted = false;
        for (;;) {
            if (pollAbort()) return false;
            double ts = 0.0;
            if (!readNative(scratch, ts, err)) return false;
            if (!scratch.empty()) {
                if (!haveFirstTs) { firstTs = ts; haveFirstTs = true; }
                natCount += (uint64_t)stampRule(ts);         // the linear decode's very rule (nativeIn)
                // (the stamps' bias is learnt here too: a later container seek needs it -- nativeIn)
                noteStartMark(ts);
                const int64_t n = (int64_t)scratch.size();
                if ((int64_t)natCount + n <= j0) { natCount += (uint64_t)n; if (srcEof) break; continue; }
                // this buffer reaches j0: restart the chain there (a gap that jumped past j0 is silence)
                delete rs;
                rs = nullptr;
                if (natRate != kRate16k) {
                    rs = new Resampler();
                    if (!rs->init(natRate, err)) return false;
                }
                rsFinished = false;
                pendF.clear();
                leadZeros = 0;
                trimLeft = 0;
                firstSample = false;
                rsK = (uint64_t)k0;
                pos = emitted = (uint64_t)(c16 + k0);
                if ((int64_t)natCount > j0) {
                    const std::vector<float> z((size_t)((int64_t)natCount - j0), 0.0f);
                    if (!toTimeline(z.data(), z.size())) { err = "The audio resampler failed."; return false; }
                }
                const size_t from = (size_t)std::max<int64_t>(0, j0 - (int64_t)natCount);
                if (!toTimeline(scratch.data() + from, scratch.size() - from)) { err = "The audio resampler failed."; return false; }
                natCount += (uint64_t)n;
                restarted = true;
                break;
            }
            if (srcEof) break;
        }
        if (!restarted) {
            // The stream ended before j0: the reader is at its end, where a linear decode would be.
            pendF.clear();
            leadZeros = 0;
            rsFinished = true;
            pos = emitted = (uint64_t)std::max<int64_t>(0, c16 + (int64_t)std::llround((double)natCount * kRate16k / natRate));
            return true;
        }
        if (srcEof) drainResampler();
        return discardTo(target, err);
    }

    // The resampler's period: Pin native samples make exactly Pout outputs. A restart on a multiple
    // of it (counted from the stream's sample 0) puts the outputs on the linear decode's very grid.
    void period(uint32_t inRate, int64_t& pin, int64_t& pout) const {
        const uint32_t g = gcdU32(inRate, kRate16k);
        pin = (int64_t)(inRate / g);
        pout = (int64_t)(kRate16k / g);
    }

    // AIFF: by byte offset (no lead, no trim, no gap: output k IS timeline frame k).
    bool seekAiff(uint64_t target, std::string& err) {
        const uint32_t nr = info.nativeRate;
        int64_t pin = 1, pout = 1;
        period(nr, pin, pout);
        int64_t k0 = (int64_t)target - (int64_t)(kSeekWarmSec * kRate16k);
        if (k0 < 0) k0 = 0;
        k0 = k0 / pout * pout;
        const uint64_t j0 = (uint64_t)(k0 / pout * pin);
        delete rs;
        rs = new Resampler();
        if (!rs->init(nr, err)) return false;
        rsFinished = false;
        srcEof = false;
        pendF.clear();
        aiffDone = std::min<uint64_t>(j0, af.frames);
        pos = emitted = (uint64_t)k0;
        seekHow = "aiff";
        return discardTo(target, err);
    }

    // The audio ENDS before `target`: the reader is put at its end -- where a linear decode would be
    // (review P3: Media Foundation landing at the end of the stream used to mean re-opening the file
    // and decoding all of it just to find that out).
    void atEnd(uint64_t target) {
        pendF.clear();
        leadZeros = 0;
        trimLeft = 0;
        firstSample = false;
        srcEof = true;
        rsFinished = true;
        pos = emitted = target;
        seekHow = "end";
    }

    // Media Foundation, on a container whose stamps were measured exact (TrMedia.h).
    enum class SeekRes { Done, Fallback, Error };
    SeekRes mfSeek(uint64_t target, std::string& err) {
        // 1. Where output 0 sits on the timeline (c16 = lead - trim), the stream's first stamp and its
        //    stamps' bias: known once the first seconds were decoded -- a seek before any read decodes
        //    that far first (a few ms). BOUNDED (C3): until a buffer is SEEN kBiasSpanSec - 0.1 past
        //    the first (marksSpan), and never more than kBiasSpanSec + 2 s of the stream.
        const uint64_t capNat = (uint64_t)((kBiasSpanSec + 2.0) * natRate);
        while (!seekedOnce && !srcEof && (c16 == kNoC16 || startMarks.empty() || !marksSpan) && natCount < capNat) {
            if (pollAbort()) return SeekRes::Error;
            std::string e2;
            if (!pullMf16(e2)) return SeekRes::Fallback;      // the decoded path will report it
        }
        if (c16 == kNoC16 || !haveFirstTs || startMarks.empty()) return SeekRes::Fallback;
        if (demoted) return SeekRes::Fallback;               // those very seconds left the count (P2)
        int64_t blo = 0, bhi = 0;
        stampOffset(startMarks, startMarks.size() > 1 ? 1 : 0, blo, bhi);   // (the first buffer: as below)
        if (bhi - blo > (int64_t)(kSeekStampSpreadSec * natRate)) { demoted = true; return SeekRes::Fallback; }
        const int64_t bias = blo + (bhi - blo) / 2;
        int64_t pin = 1, pout = 1;
        period(natRate, pin, pout);
        const int64_t warm = natRate == kRate16k ? 0 : (int64_t)(kSeekWarmSec * kRate16k);
        int64_t k0 = (int64_t)target - c16 - warm;           // the first output the restarted chain makes
        if (k0 < (int64_t)(kSeekNearStartSec * kRate16k)) return SeekRes::Fallback;
        k0 = k0 / pout * pout;
        const int64_t j0 = k0 / pout * pin;                  // ... and the native sample that feeds it
        double back = kSeekPrerollSec;
        bool endedBefore = false;                            // a landing found the stream already over
        for (int attempt = 0; attempt < 4; ++attempt, back *= 4.0) {
            const double tSeek = std::max(0.0, firstTs + (double)j0 / natRate - back);
            PROPVARIANT v;
            ::PropVariantInit(&v);
            v.vt = VT_I8;
            v.hVal.QuadPart = (LONGLONG)std::llround(tSeek * 1e7);
            const HRESULT hr = rd->SetCurrentPosition(GUID_NULL, v);
            ::PropVariantClear(&v);
            if (FAILED(hr)) return SeekRes::Fallback;
            srcEof = false;
            // 2. Decode from wherever it landed, keeping every buffer's stamp, until j0 is covered --
            //    and, when the stamps are not exact (Matroska: whole ms), as many seconds of them as the
            //    bias was measured over: the MIDRANGE of the offsets they imply, less that bias, is
            //    where the linear decode would have put them.
            std::vector<float> got;
            uint64_t gotBase = 0;                            // samples dropped from the front of `got`
            std::vector<std::pair<double, uint64_t>> marks;  // (stamp, absolute count)
            int64_t lo = 0, hi = 0;
            bool landedLate = false;
            for (;;) {
                if (pollAbort()) return SeekRes::Error;
                double ts = 0.0;
                std::string e2;
                if (!readNative(scratch, ts, e2)) return SeekRes::Fallback;
                if (!scratch.empty()) {
                    marks.emplace_back(ts, gotBase + got.size());
                    got.insert(got.end(), scratch.begin(), scratch.end());
                    // the first buffer after a flush may be partial: it votes only while it is alone
                    const int64_t o = (int64_t)std::llround((ts - firstTs) * (double)natRate) - (int64_t)marks.back().second;
                    if (marks.size() <= 2) { lo = hi = o; }
                    else { lo = std::min(lo, o); hi = std::max(hi, o); }
                    if (hi - lo > (int64_t)(kSeekStampSpreadSec * natRate)) {
                        demoted = true;                      // this file's clock is not trustworthy
                        return SeekRes::Fallback;
                    }
                    const int64_t off = lo + (hi - lo) / 2 - bias;   // native index of absolute count 0
                    if (marks.size() == 1 && off > j0) { landedLate = true; break; }
                    const int64_t have = off + (int64_t)(gotBase + got.size());
                    const double  span = marks.back().first - marks.front().first;
                    const double  need = (lo == hi && blo == bhi) ? 0.0 : kBiasSpanSec - 0.1;
                    if (have > j0 + pin && (span >= need || srcEof)) break;
                    // keep memory bounded on a landing far before the target (MPEG-TS: 40 s early)
                    if (got.size() > (size_t)natRate * 8 && off + (int64_t)gotBase + (int64_t)natRate * 4 < j0) {
                        const size_t drop = (size_t)natRate * 4;
                        got.erase(got.begin(), got.begin() + (std::ptrdiff_t)drop);
                        gotBase += drop;
                    }
                }
                if (srcEof) break;
            }
            if (landedLate) continue;                        // ask for an earlier landing
            if (marks.empty()) {
                // nothing decodes from there on: the audio ends before the landing -- try an earlier one
                // (a video whose audio track is shorter than the container); every one empty = at its end
                if (srcEof) { endedBefore = true; continue; }
                return SeekRes::Fallback;
            }
            const int64_t off = lo + (hi - lo) / 2 - bias;
            const int64_t first = off + (int64_t)gotBase;   // native index of got[0]
            if (first > j0) continue;                        // landed late after all
            if (first + (int64_t)got.size() <= j0) {
                // the stream ENDED before j0 (the loop stops only there): the target is past the audio
                if (srcEof) { atEnd(target); return SeekRes::Done; }
                return SeekRes::Fallback;
            }
            // 3. Restart the chain at j0, ON A PERIOD of the ratio, and continue as the stream does.
            delete rs;
            rs = nullptr;
            if (natRate != kRate16k) {
                rs = new Resampler();
                if (!rs->init(natRate, err)) return SeekRes::Error;
            }
            rsFinished = false;
            pendF.clear();
            leadZeros = 0;
            trimLeft = 0;
            firstSample = false;
            natCount = (uint64_t)j0;
            rsK = (uint64_t)k0;
            pos = emitted = (uint64_t)(c16 + k0);
            const size_t from = (size_t)(j0 - first);
            if (!toTimeline(got.data() + from, got.size() - from)) { err = "The audio resampler failed."; return SeekRes::Error; }
            natCount += got.size() - from;
            if (srcEof) drainResampler();
            seekHow = "mf";
            seekedOnce = true;
            return discardTo(target, err) ? SeekRes::Done : SeekRes::Error;
        }
        if (endedBefore) { atEnd(target); return SeekRes::Done; }
        return SeekRes::Fallback;
    }

    // Read up to one second of AIFF (and never more than ~4 MB) into the pending buffer.
    bool pullAiff(std::string& err) {
        if (aiffDone >= af.frames) {
            if (rs && !rsFinished) { rs->finish(pendF); rsFinished = true; }
            srcEof = true;
            return true;
        }
        const uint64_t fb = (uint64_t)af.ch * af.bytesPerSample;
        const uint64_t blk = std::max<uint64_t>(1, std::min<uint64_t>((uint64_t)std::max(1.0, af.rate),
                                                                      (4ull << 20) / fb));
        const uint64_t want = std::min<uint64_t>(af.frames - aiffDone, blk);
        std::vector<unsigned char> raw((size_t)(want * fb));
        if (!readAt(f, af.dataOff + aiffDone * fb, raw.data(), (DWORD)raw.size())) {
            err = "The AIFF file is truncated at " + mmss((double)aiffDone / af.rate) + ".";
            return false;
        }
        std::vector<float> fr(af.ch);
        auto frameAt = [&](size_t i) {
            for (uint32_t c = 0; c < af.ch; ++c) fr[c] = aiffSample(&raw[(i * af.ch + c) * af.bytesPerSample], af);
        };
        if (out == Out::Mono16k) {
            std::vector<float> mono((size_t)want);
            if (af.ch <= 2) {
                // UNCHANGED (the AIFF == WAV proof rests on it): the plain average of one or two.
                for (size_t i = 0; i < (size_t)want; ++i) {
                    double acc = 0.0;
                    for (uint32_t c = 0; c < af.ch; ++c) acc += aiffSample(&raw[(i * af.ch + c) * af.bytesPerSample], af);
                    mono[i] = (float)(acc / af.ch);
                }
            } else {
                const Fold& fo = foldFor(af.ch, 0);
                for (size_t i = 0; i < (size_t)want; ++i) { frameAt(i); foldFrame(fo, fr.data(), af.ch, 1, &mono[i]); }
            }
            if (!rs->push(mono.data(), mono.size(), pendF)) { err = "The audio resampler failed."; return false; }
            emitted = pos + pendF.size();
        } else {
            std::vector<int16_t> pcm((size_t)(want * ch));
            const Fold& fo = foldFor(af.ch, 0);
            float o[2];
            for (size_t i = 0; i < (size_t)want; ++i) {
                frameAt(i);
                if (af.ch == ch) { for (uint32_t c = 0; c < ch; ++c) o[c] = fr[c]; }
                else foldFrame(fo, fr.data(), af.ch, ch, o);
                for (uint32_t c = 0; c < ch; ++c) pcm[i * ch + c] = toPcm16(o[c]);
            }
            pendI.insert(pendI.end(), pcm.begin(), pcm.end());
            emitted += want;
        }
        aiffDone += want;
        if (inSeek) seekDecoded += want;
        return true;
    }

    static std::string mmss(double sec) {
        char b[32];
        const int s = (int)sec;
        std::snprintf(b, sizeof(b), "%d:%02d", s / 60, s % 60);
        return b;
    }
};

MediaReader::MediaReader() : d_(new Impl) {}
MediaReader::~MediaReader() = default;

const MediaInfo& MediaReader::info() const { return d_->info; }
uint32_t MediaReader::outRate() const     { return d_->rate; }
uint32_t MediaReader::outChannels() const { return d_->ch; }
bool     MediaReader::eof() const {
    return d_->srcEof && d_->pendF.empty() && d_->pendI.empty() && !d_->leadZeros;
}
uint64_t MediaReader::position() const    { return d_->pos; }

void MediaReader::close() {
    d_.reset(new Impl);
}

static IMFMediaType* audioType(const GUID& sub, uint32_t rate, uint32_t ch, uint32_t bits) {
    IMFMediaType* t = nullptr;
    ::MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    t->SetGUID(MF_MT_SUBTYPE, sub);
    t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
    t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
    t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bits);
    t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, ch * bits / 8);
    t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * ch * bits / 8);
    t->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    return t;
}

// Does this video stream carry a MOVING picture? Cover art / an attached picture is ONE frame
// (measured 10/10: Matroska exposes its attached PNG/JPEG as a video stream -- MPNG 0/0 fps,
// MJPG 25/1 fps, indistinguishable from an MJPEG video by type -- and it ends after one sample,
// where a real video delivers a second at once). Read with a reader of its own, compressed
// samples only (no decoder), on its OWN byte stream, so the audio reader's position is never
// disturbed.
static bool streamIsMoving(const std::wstring& path, DWORD streamIndex) {
    IMFByteStream* bs = nullptr;
    if (FAILED(::MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE,
                              path.c_str(), &bs)))
        return true;                                                      // assume video
    IMFSourceReader* r = nullptr;
    const HRESULT hr = ::MFCreateSourceReaderFromByteStream(bs, nullptr, &r);
    bs->Release();
    if (FAILED(hr)) return true;
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection(streamIndex, TRUE);
    int samples = 0;
    for (int k = 0; k < 3 && samples < 2; ++k) {
        DWORD si = 0, flags = 0; LONGLONG ts = 0; IMFSample* s = nullptr;
        if (FAILED(r->ReadSample(streamIndex, 0, &si, &flags, &ts, &s))) break;
        if (s) { ++samples; s->Release(); }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) break;
    }
    r->Release();
    return samples >= 2;
}

// =============================================================================
// FORMAT NAMES -- plain words: they end up in a sentence the user reads ("This sound track (DTS) can't be
// decoded..."). Before WH a DTS track printed "????" (its subtype is not a FOURCC GUID).
// =============================================================================
// The subtypes Media Foundation gives an audio stream it LISTS without decoding it (measured, WH 11/10):
// DTS in Matroska -> MFAudioFormat_DTS_HD, in an MP4 -> the 0x2001 wave tag; TrueHD -> MEDIASUBTYPE_
// DOLBY_TRUEHD; TTA / WavPack -> GUID_NULL (named from the Matroska CodecID instead). Written out here
// rather than taken from mfapi.h: several are recent there, and only a NAME is ever needed.
static const GUID kSubDtsRaw  = { 0xE06D8033, 0xDB46, 0x11CF, { 0xB4, 0xD1, 0x00, 0x80, 0x5F, 0x6C, 0xBB, 0xEA } };
static const GUID kSubDtsHd   = { 0xA2E58EB7, 0x0FA9, 0x48BB, { 0xA4, 0x0C, 0xFA, 0x0E, 0x15, 0x6D, 0x06, 0x45 } };
static const GUID kSubDtsXll  = { 0x45B37C1B, 0x8C70, 0x4E59, { 0xA7, 0xBE, 0xA1, 0xE4, 0x2C, 0x81, 0xC8, 0x0D } };
static const GUID kSubDtsLbr  = { 0xC2FE6F0A, 0x4E3C, 0x4DF1, { 0x9B, 0x60, 0x50, 0x86, 0x30, 0x91, 0xE4, 0xB9 } };
static const GUID kSubDtsUhd  = { 0x87020117, 0xACE3, 0x42DE, { 0xB7, 0x3E, 0xC6, 0x56, 0x70, 0x62, 0x63, 0xF8 } };
static const GUID kSubDtsUhdy = { 0x9B9CCA00, 0x91B9, 0x4CCC, { 0x88, 0x3A, 0x8F, 0x78, 0x7A, 0xC3, 0xCC, 0x86 } };
static const GUID kSubTrueHd  = { 0xEB27CEC4, 0x163E, 0x4CA3, { 0x8B, 0x74, 0x8E, 0x25, 0xF9, 0x1B, 0x51, 0x7E } };
static const GUID kSubDdPlus  = { 0xA7FB87AF, 0x2D02, 0x42FB, { 0xA4, 0xD4, 0x05, 0xCD, 0x93, 0x84, 0x3B, 0xDD } };
static const GUID kSubVorbis  = { 0x8D2FD10B, 0x5841, 0x4A6B, { 0x89, 0x05, 0x58, 0x8F, 0xEC, 0x1A, 0xDE, 0xD9 } };
// The Matroska source's own attribute on a stream's native type: the track's CodecID ("A_DTS", "A_TTA1",
// "A_AAC"...). Undocumented -- measured on every Matroska / WebM file tried (WH, 11/10) -- and used for a
// NAME only, never to decide anything.
static const GUID kMkvCodecIdAttr = { 0x9668DDE5, 0xC869, 0x4D6A, { 0xB0, 0x2E, 0x46, 0x3B, 0x7C, 0x23, 0x1B, 0x53 } };

// A subtype built on MFAudioFormat_Base = a WAVE format tag (DEFINE_MEDIATYPE_GUID).
static bool waveTagOf(const GUID& g, uint32_t& tag) {
    static const unsigned char kBase4[8] = { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
    if (g.Data2 != 0x0000 || g.Data3 != 0x0010 || std::memcmp(g.Data4, kBase4, 8) != 0) return false;
    tag = (uint32_t)g.Data1;
    return true;
}

static std::string mkvCodecName(const std::string& id) {
    auto starts = [&](const char* p) { return id.compare(0, std::strlen(p), p) == 0; };
    if (starts("A_DTS"))                         return "DTS";
    if (starts("A_TRUEHD") || starts("A_MLP"))   return "Dolby TrueHD";
    if (starts("A_TTA"))                         return "TTA";
    if (starts("A_WAVPACK"))                     return "WavPack";
    if (starts("A_EAC3"))                        return "Dolby Digital Plus (E-AC-3)";
    if (starts("A_AC3"))                         return "Dolby Digital (AC-3)";
    if (starts("A_AAC"))                         return "AAC";
    if (starts("A_MPEG/L3"))                     return "MP3";
    if (starts("A_MPEG/"))                       return "MPEG audio";
    if (starts("A_FLAC"))                        return "FLAC";
    if (starts("A_OPUS"))                        return "Opus";
    if (starts("A_VORBIS"))                      return "Vorbis";
    if (starts("A_ALAC"))                        return "ALAC";
    if (starts("A_PCM"))                         return "PCM";
    if (starts("A_REAL/"))                       return "RealAudio";
    if (starts("A_QUICKTIME"))                   return "QuickTime audio";
    if (starts("A_MS/ACM"))                      return "an ACM codec";
    std::string s;                               // anything else: the id itself, printable and short
    for (size_t k = starts("A_") ? 2 : 0; k < id.size() && s.size() < 24; ++k)
        if (id[k] >= 32 && id[k] <= 126) s += id[k];
    return s;
}

static std::string codecName(const GUID& g, const std::string& mkvCodec = std::string()) {
    uint32_t tag = 0;
    if (waveTagOf(g, tag)) {
        switch (tag) {
            case 0x0001: return "PCM";
            case 0x0003: return "Float";
            case 0x0002: return "MS ADPCM";
            case 0x0011: return "IMA ADPCM";
            case 0x0006: return "A-law";
            case 0x0007: return "mu-law";
            case 0x0008: case 0x2001: return "DTS";
            case 0x0050: return "MPEG audio";
            case 0x0055: return "MP3";
            case 0x0092: return "Dolby Digital (AC-3)";
            case 0x00FF: case 0x1600: case 0x1610: return "AAC";
            case 0x000A: return "WMA Voice";
            case 0x0160: case 0x0161: return "WMA";
            case 0x0162: return "WMA Pro";
            case 0x0163: return "WMA Lossless";
            case 0x7361: return "AMR";
            case 0x7362: case 0x7363: return "AMR-WB";
            case 0xF1AC: return "FLAC";
            case 0x704F: return "Opus";
            case 0x6C61: return "ALAC";
            case 0xAC40: return "Dolby AC-4";
            default: break;
        }
    }
    if (g == MFAudioFormat_Dolby_AC3) return "Dolby Digital (AC-3)";
    if (g == kSubDdPlus)              return "Dolby Digital Plus (E-AC-3)";
    if (g == kSubDtsRaw || g == kSubDtsHd || g == kSubDtsXll || g == kSubDtsLbr || g == kSubDtsUhd || g == kSubDtsUhdy)
        return "DTS";
    if (g == kSubTrueHd)              return "Dolby TrueHD";
    if (g == kSubVorbis)              return "Vorbis";
    if (!mkvCodec.empty()) {
        const std::string n = mkvCodecName(mkvCodec);
        if (!n.empty()) return n;
    }
    // a FOURCC-built subtype ("alac", "samr"...) reads as one; anything else has no name here
    char fc[5] = { (char)(g.Data1 & 0xff), (char)((g.Data1 >> 8) & 0xff), (char)((g.Data1 >> 16) & 0xff),
                   (char)((g.Data1 >> 24) & 0xff), 0 };
    for (int k = 0; k < 4; ++k) if (fc[k] < 32 || fc[k] > 126) return "an unknown format";
    return fc;
}

// An ISO-BMFF sample entry's type, named -- for a sound trak Media Foundation does not list at all (measured:
// the 'alac' trak of an MP4), so no subtype exists to name it by.
static std::string isoEntryName(const char* e) {
    const std::string s(e);
    if (s == "mp4a") return "MPEG-4 audio";
    if (s == "alac") return "ALAC";
    if (s == "mlpa") return "Dolby TrueHD";
    if (s == "ac-3") return "Dolby Digital (AC-3)";
    if (s == "ec-3") return "Dolby Digital Plus (E-AC-3)";
    if (s == "ac-4") return "Dolby AC-4";
    if (s == "dtsc" || s == "dtsh" || s == "dtsl" || s == "dtse" || s == "dtsx" || s == "dtsy") return "DTS";
    if (s == "Opus") return "Opus";
    if (s == "fLaC") return "FLAC";
    if (s == ".mp3") return "MP3";
    if (s == "samr") return "AMR";
    if (s == "sawb") return "AMR-WB";
    if (s == "sowt" || s == "twos" || s == "lpcm" || s == "ipcm" || s == "fpcm" || s == "in24" || s == "in32"
        || s == "fl32" || s == "fl64" || s == "raw " || s == "NONE") return "PCM";
    if (s == "ulaw") return "mu-law";
    if (s == "alaw") return "A-law";
    if (s == "ima4") return "IMA ADPCM";
    if (s.size() != 4 || s == "????") return "an unknown format";
    return s;
}

// The sentence for a sound track that cannot be decoded: a file with other sound tracks says so (choose another
// one); a file with one keeps the pre-WH advice -- and so does the FPROXY copy, which offers no choice.
static std::string cannotDecodeSentence(const std::string& format, bool otherTracks) {
    if (otherTracks)
        return "This sound track (" + format + ") can't be decoded for the analysis. Choose another sound track.";
    return "This file's audio (" + format + ") cannot be decoded here. Convert it to WAV, MP3 or M4A.";
}

// =============================================================================
// WHICH SOUND TRACK (wave WH, 2026-10-11)
//
// The tray names a track the way the CONTAINER does (TrPipe.h: ord = its place among the container's sound
// tracks, undecodable ones included; trackId = Matroska TrackNumber / ISO-BMFF track_ID). Media Foundation
// numbers them its own way, so the link is made from evidence, MEASURED with the product's own open
// (MFCreateFile + MFCreateSourceReaderFromByteStream, no attributes) on wh\media\ (mkv_2a_def2, mp4_2a_movtext,
// mkv_dts_first, webm_vtt), 24 variants muxed for it (audio first, zh first, 4 sound tracks, track_IDs
// 10/7/3, mov_text in the middle, AC-3, DTS, TrueHD, TTA, WavPack, Vorbis+FLAC, Opus, ALAC, MOV PCM, m4a,
// mka) and the user's film (HEVC + 5 AAC + 8 ASS/VobSub + fonts) -- the log: WH_PROGRESS.md, WH-H, in
// scratchpad\filemode_2026-10-10:
//
//  * MATROSKA / WebM (12 files): the reader lists the video and EVERY audio TrackEntry, in TrackEntry order,
//    and nothing else (0 of the film's 8 subtitle tracks, no attachment); a stream's identifier
//    (IMFStreamDescriptor::GetStreamIdentifier, the presentation descriptor's index = the reader's) IS the
//    TrackNumber; a track it cannot decode is STILL listed (A_DTS as DTS_HD, A_TRUEHD, A_TTA1 / A_WAVPACK4
//    with a null subtype), so the order never skips one. Its FIRST_AUDIO_STREAM, and its default, is the
//    first audio track, FlagDefault ignored (zh flagged default, en first -> en) -- what Chromium plays too.
//    => trackId -> the stream with that identifier (a TrackNumber it does not list is an ERROR, never a
//    guess; found at another place than `ord`, the id wins: the container's own number); trackId 0 -> the
//    ord-th audio stream; "" -> the first one (pre-WH, bit for bit).
//  * ISO-BMFF (mp4 m4a m4v mov 3gp; 17 files): the MP4 source lists the sound traks FIRST, in REVERSE moov
//    order, then the video (traks v,2,3,4,5 -> [5,4,3,2,v]), under identifiers 1..n of ITS order -- never
//    the track_IDs (10/7/3 -> 3,2,1) -- and its FIRST_AUDIO_STREAM / default pick is its own (the German
//    track of four, an AAC over an enabled AC-3...), not the first sound trak, which Chromium plays. What
//    ties a stream to its trak EXACTLY: the native type's MF_MT_MPEG4_SAMPLE_DESCRIPTION is that trak's
//    whole 'stsd' box, byte for byte (every audio stream of the 17 files), and it is unique per trak where
//    the sample entry carries an ES_ID (every AAC mp4 / m4a / mov: even ONE stream muxed twice differs).
//    And a sound trak it cannot read may be MISSING altogether, not listed undecodable (the 'alac'
//    trak of an MP4 -- while 'mlpa' TrueHD and DTS are listed with a subtype nothing decodes).
//    => the trak is chosen in OUR moov table (track_ID, else the ord-th sound trak), then the ONE stream
//    whose sample description equals its stsd; several (MOV PCM 'sowt': two identical 76-byte boxes) or
//    none -> the measured reverse order, trusted only when MF lists exactly as many audio streams as there
//    are sound traks AND every sample description it gives agrees with that pairing. No stream, while every
//    listed one carries its description (the link demonstrably works) -> Windows does not list it: "can't
//    be decoded", named by its sample entry. For the FILE job "" is that same first sound trak or that
//    same error ("" == "0:<id>", TrPipe.h) -- never Media Foundation's own pick under ord 0's name; only
//    where the link itself is missing (no description at all, a moov not read) does "" fall back to
//    FIRST_AUDIO_STREAM, as before WH -- and so does the FPROXY copy, which must play SOMETHING.
//    The edit list applied is the DECODED trak's own -- pre-WH it was always the first sound trak's, even
//    when Media Foundation decoded another one.
//  * Every other container (WAVE, MP3, FLAC, Ogg, AVI, ASF, MPEG-TS...) has no track list on the tray's side:
//    "" and "0:<id>" -> FIRST_AUDIO_STREAM exactly as before; a later track is refused (an order nobody
//    measured is not guessed). AIFF (our parser) has one track.
// =============================================================================
struct MfAudioStream {
    DWORD       index = 0;                     // the Source Reader's
    GUID        sub = GUID_NULL;
    UINT32      rate = 0, ch = 0, mask = 0;
    bool        haveId = false;
    DWORD       id = 0;                        // IMFStreamDescriptor::GetStreamIdentifier
    std::vector<unsigned char> stsd;           // MF_MT_MPEG4_SAMPLE_DESCRIPTION (ISO-BMFF)
    std::string mkvCodec;                      // the Matroska CodecID attribute
    std::string lang, name;                    // MF_SD_LANGUAGE, MF_SD_STREAM_NAME
};

static std::string wideToUtf8Capped(const wchar_t* w, size_t maxChars) {
    if (!w) return std::string();
    size_t n = 0;
    while (n < maxChars && w[n]) ++n;
    if (!n) return std::string();
    const int k = ::WideCharToMultiByte(CP_UTF8, 0, w, (int)n, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(k > 0 ? k : 0), '\0');
    if (k > 0) ::WideCharToMultiByte(CP_UTF8, 0, w, (int)n, &s[0], k, nullptr, nullptr);
    return s;
}

// A string attribute, "" when absent (capped: these are names).
static std::string stringAttr(IMFAttributes* a, const GUID& key) {
    UINT32 len = 0;
    if (!a || FAILED(a->GetStringLength(key, &len)) || len > 4096) return std::string();
    std::vector<wchar_t> w((size_t)len + 1, L'\0');
    if (FAILED(a->GetString(key, w.data(), len + 1, &len))) return std::string();
    return wideToUtf8Capped(w.data(), 256);
}

// A stream descriptor's string attribute through the reader (MF_SD_LANGUAGE, MF_SD_STREAM_NAME).
static std::string streamString(IMFSourceReader* rd, DWORD index, const GUID& key) {
    PROPVARIANT v;
    ::PropVariantInit(&v);
    std::string s;
    if (SUCCEEDED(rd->GetPresentationAttribute(index, key, &v)) && v.vt == VT_LPWSTR)
        s = wideToUtf8Capped(v.pwszVal, 256);
    ::PropVariantClear(&v);
    return s;
}

// The presentation descriptor's (major type, identifier) per index -- the reader's index IS the descriptor's
// (measured: same count, same types, every file). Empty when the source cannot be asked.
static void streamIdentifiers(IMFSourceReader* rd, std::vector<std::pair<GUID, DWORD>>& out) {
    out.clear();
    IMFMediaSource* src = nullptr;
    if (FAILED(rd->GetServiceForStream((DWORD)MF_SOURCE_READER_MEDIASOURCE, GUID_NULL, IID_PPV_ARGS(&src))))
        return;
    IMFPresentationDescriptor* pd = nullptr;
    if (SUCCEEDED(src->CreatePresentationDescriptor(&pd))) {
        DWORD n = 0;
        pd->GetStreamDescriptorCount(&n);
        for (DWORD i = 0; i < n && i < 64; ++i) {
            BOOL sel = FALSE;
            IMFStreamDescriptor* sd = nullptr;
            GUID major = GUID_NULL;
            DWORD id = 0;
            if (SUCCEEDED(pd->GetStreamDescriptorByIndex(i, &sel, &sd))) {
                sd->GetStreamIdentifier(&id);
                IMFMediaTypeHandler* h = nullptr;
                if (SUCCEEDED(sd->GetMediaTypeHandler(&h))) { h->GetMajorType(&major); release(h); }
                release(sd);
            }
            out.emplace_back(major, id);
        }
        release(pd);
    }
    release(src);
}

// Where MF_SOURCE_READER_FIRST_AUDIO_STREAM is in `aud`: selected alone, it is the one the reader says is
// selected (measured: the first audio stream Media Foundation selects by DEFAULT -- not the first by index).
static int firstAudioPos(IMFSourceReader* rd, const std::vector<MfAudioStream>& aud) {
    rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(rd->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE)))
        for (size_t k = 0; k < aud.size(); ++k) {
            BOOL on = FALSE;
            if (SUCCEEDED(rd->GetStreamSelection(aud[k].index, &on)) && on) return (int)k;
        }
    return 0;
}

// The rule above. `pos` = the chosen stream's place in `aud`; `trak` = its ISO-BMFF trak (edit list), or
// null; `ord` / `id` = what it is in the container's terms (-1 / 0 = not known). false = `err`, a sentence.
// `strict` (the FILE job): "" is the first sound trak or an error, as "0:<id>" is -- never Media Foundation's
// own pick under ord 0's name; the FPROXY copy (not strict) keeps the pre-WH fallback: a sound to hear.
static bool pickAudio(Container kind, bool isoTable, const std::vector<TrakFacts>& traks,
                      const std::vector<MfAudioStream>& aud, const AudioTrack& want, IMFSourceReader* rd, bool strict,
                      int& pos, const TrakFacts*& trak, int& ord, uint32_t& id, std::string& how, std::string& err) {
    pos = -1; trak = nullptr; ord = -1; id = 0; how.clear();
    auto notFound = [&]() {
        err = "The chosen sound track (" + (want.trackId ? "#" + std::to_string(want.trackId)
                                                         : "number " + std::to_string((unsigned long long)want.ord + 1))
            + ") is not in this file as Windows reads it.";
        return false;
    };
    if (kind == Container::Ebml) {
        if (!want.chosen) {
            pos = 0;
            how = "mkv-first";
        } else if (want.trackId) {
            bool ids = true;
            for (const auto& s : aud) ids = ids && s.haveId;
            if (ids) {
                for (size_t k = 0; k < aud.size() && pos < 0; ++k) if (aud[k].id == want.trackId) pos = (int)k;
                if (pos < 0) return notFound();
                how = "mkv-id";
            } else {
                // (no identifier at all -- never measured: the order, which never skips a track, M1)
                if (want.ord >= aud.size()) return notFound();
                pos = (int)want.ord;
                how = "mkv-ord";
            }
        } else {
            if (want.ord >= aud.size()) return notFound();
            pos = (int)want.ord;
            how = "mkv-ord";
        }
        ord = pos;                                     // Matroska: MF's audio order IS the container's
        id = aud[(size_t)pos].haveId ? aud[(size_t)pos].id : want.trackId;
        return true;
    }
    if (kind == Container::Iso && isoTable) {
        std::vector<size_t> sound;                     // traks[] of the sound traks, moov order = ord
        for (size_t k = 0; k < traks.size(); ++k) if (traks[k].sound) sound.push_back(k);
        int so = -1;                                   // the wanted trak's place in `sound`
        if (!want.chosen) {
            if (!sound.empty()) so = 0;
        } else if (want.trackId) {
            for (size_t k = 0; k < sound.size() && so < 0; ++k)
                if (traks[sound[k]].trackId == want.trackId) so = (int)k;
        } else if (want.ord < sound.size()) {
            so = (int)want.ord;
        }
        if (so >= 0) {
            const TrakFacts& t = traks[sound[(size_t)so]];
            int hit = -1, hits = 0;
            if (!t.stsd.empty())
                for (size_t k = 0; k < aud.size(); ++k) if (aud[k].stsd == t.stsd) { hit = (int)k; ++hits; }
            if (hits == 1) {
                pos = hit;
                how = "iso-stsd";
            } else if (aud.size() == sound.size()) {
                bool agree = true;
                for (size_t k = 0; k < aud.size(); ++k)
                    if (!aud[k].stsd.empty() && aud[k].stsd != traks[sound[sound.size() - 1 - k]].stsd) agree = false;
                if (agree) {
                    pos = (int)(sound.size() - 1 - (size_t)so);
                    how = "iso-order";
                }
            }
            if (pos >= 0) {
                trak = &t;
                ord = so;
                id = t.trackId;
                return true;
            }
            // The trak IS there (our table) and no stream is it. When every stream Media Foundation lists carries
            // its sample description -- the link works on this system -- it simply does not list this one
            // (measured: the 'alac' trak of an MP4, gone from the presentation): Windows cannot read it.
            // (Several identical descriptions and no order to trust: it cannot be told apart -- never a guess.)
            bool linked = true;
            for (const auto& s : aud) linked = linked && !s.stsd.empty();
            if (linked && (want.chosen || strict)) {
                err = hits == 0 ? cannotDecodeSentence(isoEntryName(t.entry), sound.size() >= 2)
                                : std::string("This sound track can't be told apart from another one of the file "
                                              "as Windows reads it.");
                return false;
            }
        }
        if (want.chosen) return notFound();
    } else if (want.chosen && want.ord > 0) {
        // an ISO-BMFF moov that could not be read, and every container the tray lists no tracks for: an order
        // nobody measured is not guessed
        if (kind == Container::Iso) return notFound();
        err = "Only the first sound track of this kind of file can be analysed.";
        return false;
    }
    // "the first sound track" where nothing above applies: Media Foundation's own, as before WH
    pos = firstAudioPos(rd, aud);
    how = "first";
    if (aud.size() == 1) ord = 0;
    if (isoTable) {
        // its trak, for the edit list: the one whose stsd it carries -- else the first sound trak (pre-WH)
        const TrakFacts* only = nullptr;
        int hits = 0, so = -1, k = 0;
        for (const auto& t : traks) {
            if (!t.sound) continue;
            if (!t.stsd.empty() && t.stsd == aud[(size_t)pos].stsd) { only = &t; ++hits; so = k; }
            ++k;
        }
        if (hits == 1) { trak = only; ord = so; id = only->trackId; }
        else for (const auto& t : traks) if (t.sound) { trak = &t; break; }
    }
    return true;
}

bool MediaReader::open(const std::wstring& path, Out out, std::string& err) {
    return open(path, out, AudioTrack(), err);
}

bool MediaReader::open(const std::wstring& path, Out out, const AudioTrack& track, std::string& err) {
    close();
    Impl& d = *d_;
    d.out = out;
    d.path = path;
    d.track = track;
    err.clear();
    if (!d.mf.init(err)) return false;

    // ---- AIFF first: Media Foundation cannot open it, and the sniff is 12 bytes ------------
    {
        HANDLE f = openShared(path);
        if (f == INVALID_HANDLE_VALUE) {
            const DWORD e = ::GetLastError();
            err = (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND)
                ? "The file was not found."
                : "The file could not be opened (it may be locked or not readable).";
            return false;
        }
        if (sniffAiff(f)) {
            d.f = f;
            if (track.chosen && track.ord > 0) {           // one sound track, by construction
                d.closeAll();
                err = "Only the first sound track of this kind of file can be analysed.";
                return false;
            }
            if (!parseAiff(f, d.af, err)) { d.closeAll(); return false; }
            d.info.aiff           = true;
            d.info.nativeRate     = (uint32_t)std::lround(d.af.rate);
            d.info.nativeChannels = d.af.ch;
            d.info.durationMs     = (uint64_t)std::llround((double)d.af.frames * 1000.0 / d.af.rate);
            d.info.codec          = "AIFF " + d.af.compName + " " + std::to_string(d.af.bits) + "-bit";
            d.info.seekMode       = "aiff";
            d.info.audioStreams   = 1;
            d.info.trackOrd       = 0;
            d.info.trackHow       = "aiff";
            if (out == Out::Mono16k) {
                d.rate = kRate16k; d.ch = 1;
                d.rs = new Resampler();
                if (!d.rs->init(d.info.nativeRate, err)) { d.closeAll(); return false; }
            } else {
                d.rate = d.info.nativeRate; d.ch = std::min<uint32_t>(d.af.ch, 2);
            }
            return true;
        }
        ::CloseHandle(f);
    }

    // ---- Media Foundation -------------------------------------------------------------------
    IMFByteStream* bs = nullptr;
    HRESULT hr = ::MFCreateFile(MF_ACCESSMODE_READ, MF_OPENMODE_FAIL_IF_NOT_EXIST, MF_FILEFLAGS_NONE,
                                path.c_str(), &bs);
    if (FAILED(hr)) { err = "The file could not be opened (" + hr8(hr) + ")."; return false; }
    hr = ::MFCreateSourceReaderFromByteStream(bs, nullptr, &d.rd);
    if (FAILED(hr)) {
        release(bs);
        err = "Windows cannot read this file's format. Convert it to WAV, MP3 or M4A.";
        return false;
    }
    // Probe: the streams (every audio stream's facts -- WH: the choice is made among them -- and picture
    // or not) and the duration. BOUNDED (WB, after the review): a source that fails every index with
    // something other than "no such stream" would otherwise be asked for ever.
    std::vector<std::pair<GUID, DWORD>> pdIds;
    streamIdentifiers(d.rd, pdIds);
    std::vector<MfAudioStream> aud;                  // the AUDIO streams, in the reader's index order
    for (DWORD i = 0; i < 64; ++i) {
        IMFMediaType* t = nullptr;
        hr = d.rd->GetNativeMediaType(i, 0, &t);
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        if (FAILED(hr)) continue;
        GUID major{};
        t->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Audio) {
            MfAudioStream s;
            s.index = i;
            t->GetGUID(MF_MT_SUBTYPE, &s.sub);
            t->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &s.rate);
            t->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &s.ch);
            if (FAILED(t->GetUINT32(MF_MT_AUDIO_CHANNEL_MASK, &s.mask))) s.mask = 0;
            UINT32 bl = 0;
            if (SUCCEEDED(t->GetBlobSize(MF_MT_MPEG4_SAMPLE_DESCRIPTION, &bl)) && bl && bl <= (1u << 20)) {
                s.stsd.resize(bl);
                if (FAILED(t->GetBlob(MF_MT_MPEG4_SAMPLE_DESCRIPTION, s.stsd.data(), bl, &bl))) s.stsd.clear();
                else s.stsd.resize(bl);
            }
            s.mkvCodec = stringAttr(t, kMkvCodecIdAttr);
            if (i < pdIds.size() && pdIds[i].first == major) { s.haveId = true; s.id = pdIds[i].second; }
            s.lang = streamString(d.rd, i, MF_SD_LANGUAGE);
            s.name = streamString(d.rd, i, MF_SD_STREAM_NAME);
            aud.push_back(std::move(s));
        } else if (major == MFMediaType_Video && !d.info.hasVideo) {
            d.info.hasVideo = streamIsMoving(path, i);
        }
        release(t);
    }
    release(bs);
    if (aud.empty()) {
        d.closeAll();
        err = "This file has no audio track.";
        return false;
    }
    d.info.audioStreams = (uint32_t)aud.size();

    // ---- WHICH SOUND TRACK (above) ------------------------------------------------------------
    const Container kind = sniffContainer(path);
    std::vector<TrakFacts> traks;
    uint32_t mvhdScale = 0;
    const bool isoTable = kind == Container::Iso && isoBmffTraks(path, traks, mvhdScale);
    int pos = -1, ord = -1;
    uint32_t id = 0;
    const TrakFacts* trak = nullptr;
    std::string how;
    if (!pickAudio(kind, isoTable, traks, aud, track, d.rd, out == Out::Mono16k, pos, trak, ord, id, how, err)) {
        d.closeAll();
        return false;
    }
    const MfAudioStream& s = aud[(size_t)pos];
    d.aIdx                = s.index;
    d.info.streamIndex    = s.index;
    d.info.trackOrd       = ord;
    d.info.trackId        = id;
    d.info.trackLang      = s.lang;
    d.info.trackName      = s.name;
    d.info.trackHow       = how;
    d.info.nativeRate     = s.rate;
    d.info.nativeChannels = s.ch;
    d.info.codec          = codecName(s.sub, s.mkvCodec);
    // ONE sentence when it cannot be decoded (cannotDecodeSentence): the FPROXY copy has no choice to offer.
    auto cannotDecode = [&]() {
        d.closeAll();
        err = cannotDecodeSentence(d.info.codec, out == Out::Mono16k && aud.size() >= 2);
        return false;
    };
    if (!s.rate || !s.ch) return cannotDecode();      // listed, but nothing Media Foundation can describe
    {
        PROPVARIANT v; ::PropVariantInit(&v);
        if (SUCCEEDED(d.rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE,
                                                     MF_PD_DURATION, &v)))
            d.info.durationMs = (uint64_t)(v.uhVal.QuadPart / 10000ull);
        ::PropVariantClear(&v);
    }
    // ISO-BMFF / QuickTime: the edit list MF ignores (TrMedia.h) -- the DECODED trak's (WH).
    if (trak && isoEditOf(*trak, mvhdScale, d.info.leadSec, d.info.trimSec)) {
        d.shiftSec = d.info.leadSec - d.info.trimSec;
    }

    d.rd->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    d.rd->SetStreamSelection(d.aIdx, TRUE);
    if (out == Out::Mono16k) {
        // WD: MONO at the NATIVE rate, and our resampler to 16 kHz (the members' comment): measured
        // bit-identical to asking Media Foundation for float/16k/mono, and it can be restarted on a
        // seek without moving the output grid.
        d.rate = kRate16k; d.ch = 1;
        d.natRate = d.info.nativeRate;
        d.mfMono16 = true;
        IMFMediaType* t = audioType(MFAudioFormat_Float, d.natRate, 1, 32);
        hr = d.rd->SetCurrentMediaType(d.aIdx, nullptr, t);
        release(t);
        if (FAILED(hr)) return cannotDecode();
        if (d.natRate != kRate16k) {
            d.rs = new Resampler();
            if (!d.rs->init(d.natRate, err)) { d.closeAll(); return false; }
        }
        d.seekTrusted = seekTrustedContainer(kind);
        d.info.seekMode = d.seekTrusted ? "mf" : "decode";
    } else {
        d.rate = d.info.nativeRate;
        d.ch   = std::min<uint32_t>(d.info.nativeChannels, 2);
        IMFMediaType* t = audioType(MFAudioFormat_PCM, d.rate, d.ch, 16);
        hr = d.rd->SetCurrentMediaType(d.aIdx, nullptr, t);
        release(t);
        if (FAILED(hr)) {
            // MF would not fold this layout to <= 2 channels: take float at the native layout,
            // which consumeMf folds by speaker position.
            IMFMediaType* f = audioType(MFAudioFormat_Float, d.rate, d.info.nativeChannels, 32);
            hr = d.rd->SetCurrentMediaType(d.aIdx, nullptr, f);
            release(f);
            if (FAILED(hr)) return cannotDecode();
        }
    }
    d.readCurrentType();                    // what MF will actually deliver
    if (!d.curMask) d.curMask = s.mask;
    if (d.info.trimSec > 0.0) d.trimLeft = (uint64_t)std::llround(d.info.trimSec * d.rate);
    return true;
}

bool MediaReader::readFloat(std::vector<float>& outv, size_t maxFrames, std::string& err) {
    Impl& d = *d_;
    if (d.out != Out::Mono16k) { err = "internal: readFloat on a PCM16 reader"; return false; }
    if (d.broken) { err = "internal: a read after an aborted seek (seek again first)"; return false; }
    if (!d.rd && d.f == INVALID_HANDLE_VALUE) { err = "internal: a read from a reader that is not open"; return false; }
    while (d.pendF.size() < maxFrames && !d.srcEof) {
        if (!d.pull(err)) return false;
    }
    if (d.leadZeros) {                      // the lead first, from its counter (place())
        const size_t z = (size_t)std::min<uint64_t>(d.leadZeros, maxFrames);
        outv.insert(outv.end(), z, 0.0f);
        d.leadZeros -= z;
        d.pos += z;
        return true;
    }
    const size_t n = std::min(maxFrames, d.pendF.size());
    outv.insert(outv.end(), d.pendF.begin(), d.pendF.begin() + (std::ptrdiff_t)n);
    d.pendF.erase(d.pendF.begin(), d.pendF.begin() + (std::ptrdiff_t)n);
    d.pos += n;
    return true;
}

bool MediaReader::readPcm16(std::vector<int16_t>& outv, size_t maxFrames, std::string& err) {
    Impl& d = *d_;
    if (d.out != Out::Pcm16Native) { err = "internal: readPcm16 on a float reader"; return false; }
    while (d.pendI.size() < maxFrames * d.ch && !d.srcEof) {
        if (!(d.f != INVALID_HANDLE_VALUE ? d.pullAiff(err) : d.pullMf(err))) return false;
    }
    if (d.leadZeros) {                      // the lead first, from its counter (place())
        const size_t z = (size_t)std::min<uint64_t>(d.leadZeros, maxFrames);
        outv.insert(outv.end(), z * d.ch, (int16_t)0);
        d.leadZeros -= z;
        d.pos += z;
        return true;
    }
    const size_t n = std::min(maxFrames, d.pendI.size() / d.ch);
    outv.insert(outv.end(), d.pendI.begin(), d.pendI.begin() + (std::ptrdiff_t)(n * d.ch));
    d.pendI.erase(d.pendI.begin(), d.pendI.begin() + (std::ptrdiff_t)(n * d.ch));
    d.pos += n;
    return true;
}

// WD: see TrMedia.h. The cheap cases first (already there, inside what is decoded, a little ahead),
// then the container's own seek where it is trusted, and DECODING everywhere else -- forward, or
// from a fresh open when the target is behind. Every decoding loop polls `abort` (review P3).
bool MediaReader::seek(uint64_t frame, std::string& err, const std::function<bool()>& abort) {
    err.clear();
    if (d_->out != Out::Mono16k) { err = "internal: seek on a PCM16 reader"; return false; }
    if (!d_->rd && d_->f == INVALID_HANDLE_VALUE) { err = "internal: seek on a reader that is not open"; return false; }
    // While a seek runs, the Impl polls `abort` and counts what it decodes; a re-open replaces the
    // Impl, so this is set on whichever one is current, and cleared on the way out.
    auto arm = [&](Impl& d) { d.abortFn = &abort; d.inSeek = true; d.aborted = false; };
    auto done = [&](bool ok) {
        Impl& d = *d_;
        d.abortFn = nullptr;
        d.inSeek = false;
        if (d.aborted) { err.clear(); return false; }
        return ok;
    };
    uint64_t decodedBefore = 0;                // what a re-open threw away still counts as this seek's cost
    if (!d_->broken) {
        Impl& d = *d_;
        arm(d);
        d.seekDecoded = 0;
        if (frame == d.pos) { d.seekHow = "none"; return done(true); }
        if (frame > d.pos && frame - d.pos <= d.leadZeros + d.pendF.size()) {
            d.seekHow = "pending";
            return done(d.discardTo(frame, err));
        }
        if (d.f != INVALID_HANDLE_VALUE) return done(d.seekAiff(frame, err));
        const uint64_t nearStart = (uint64_t)(kSeekNearStartSec * kRate16k);
        const uint64_t forward   = (uint64_t)(kSeekForwardSec * kRate16k);
        const bool     trusted   = d.seekTrusted && !d.demoted;
        if (frame > d.pos && (frame - d.pos <= forward || !trusted || frame < nearStart)) {
            d.seekHow = "forward";
            return done(frame - d.pos > forward ? d.fastForward(frame, err) : d.discardTo(frame, err));
        }
        if (trusted && frame >= nearStart) {
            const Impl::SeekRes r = d.mfSeek(frame, err);
            if (r == Impl::SeekRes::Done)  return done(true);
            if (r == Impl::SeekRes::Error) return done(false);
        }
        decodedBefore = d.seekDecoded;
        d.abortFn = nullptr;
        d.inSeek = false;
    } else {
        decodedBefore = 0;
    }
    // DECODED: from a fresh open (the target is behind, near the start, Media Foundation's landing
    // could not be anchored, or the last seek was aborted half way). A file demoted on its own stamps
    // stays demoted -- and the re-open decodes THE SAME sound track (WH).
    const bool demoted = d_->demoted;
    const std::wstring path = d_->path;
    const AudioTrack track = d_->track;
    if (!open(path, Out::Mono16k, track, err)) return false;
    if (demoted) { d_->demoted = true; d_->info.seekMode = "decode"; }
    arm(*d_);
    d_->seekDecoded = decodedBefore;
    const bool start = frame < (uint64_t)(kSeekNearStartSec * kRate16k);
    d_->seekHow = start ? "start" : "reopen";
    return done(start ? d_->discardTo(frame, err) : d_->fastForward(frame, err));
}

const char* MediaReader::lastSeekHow() const { return d_->seekHow; }
bool        MediaReader::lastSeekAborted() const { return d_->aborted; }
uint64_t    MediaReader::lastSeekDecoded() const { return d_->seekDecoded; }

// =============================================================================
// FPROXY
// =============================================================================
static uint64_t g_rf64ThresholdForTest = 0;
void setProxyRf64ThresholdForTest(uint64_t dataBytes) { g_rf64ThresholdForTest = dataBytes; }
void setMediaFoundationDllForTest(const std::wstring& dll) { g_mfDllForTest = dll; }

namespace {
void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
void put32(unsigned char* p, uint32_t v) { for (int k = 0; k < 4; ++k) p[k] = (unsigned char)(v >> (8 * k)); }
void put64(unsigned char* p, uint64_t v) { for (int k = 0; k < 8; ++k) p[k] = (unsigned char)(v >> (8 * k)); }

bool writeAll(HANDLE h, const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*)p;
    while (n) {
        const DWORD k = (DWORD)std::min<size_t>(n, 1u << 30);
        DWORD w = 0;
        if (!::WriteFile(h, b, k, &w, nullptr) || w != k) return false;
        b += k; n -= k;
    }
    return true;
}
} // namespace

// The header, 80 bytes: RIFF/WAVE, a 28-byte JUNK chunk reserved for RF64's ds64 (the
// standard trick: a reader that knows neither skips JUNK), fmt, then data.
static void wavHeader(unsigned char* h, bool rf64, uint32_t rate, uint32_t ch, uint64_t dataBytes,
                      uint64_t frames) {
    std::memset(h, 0, 80);
    const uint64_t riff = 72 + dataBytes;
    std::memcpy(h, rf64 ? "RF64" : "RIFF", 4);
    put32(h + 4, rf64 ? 0xFFFFFFFFu : (uint32_t)riff);
    std::memcpy(h + 8, "WAVE", 4);
    std::memcpy(h + 12, rf64 ? "ds64" : "JUNK", 4);
    put32(h + 16, 28);
    if (rf64) { put64(h + 20, riff); put64(h + 28, dataBytes); put64(h + 36, frames); put32(h + 44, 0); }
    std::memcpy(h + 48, "fmt ", 4);
    put32(h + 52, 16);
    put16(h + 56, 1);                         // PCM
    put16(h + 58, (uint16_t)ch);
    put32(h + 60, rate);
    put32(h + 64, rate * ch * 2);
    put16(h + 68, (uint16_t)(ch * 2));
    put16(h + 70, 16);
    std::memcpy(h + 72, "data", 4);
    put32(h + 76, rf64 ? 0xFFFFFFFFu : (uint32_t)dataBytes);
}

ProxyResult writeProxyWav(const std::wstring& src, const std::wstring& out,
                          const std::atomic<bool>& cancel, std::string& err, ProxyFacts* facts) {
    err.clear();
    MediaReader r;
    if (!r.open(src, MediaReader::Out::Pcm16Native, err)) return ProxyResult::Error;
    const uint32_t rate = r.outRate(), ch = r.outChannels();
    const std::wstring tmp = out + L".tmp";
    HANDLE h = ::CreateFileW(tmp.c_str(), GENERIC_WRITE | GENERIC_READ, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "The playable copy could not be created (" + std::to_string(::GetLastError()) + ").";
        return ProxyResult::Error;
    }
    auto abandon = [&](ProxyResult res) {
        ::CloseHandle(h);
        deleteWithRetry(tmp);
        return res;
    };
    unsigned char hdr[80];
    wavHeader(hdr, false, rate, ch, 0, 0);
    if (!writeAll(h, hdr, sizeof(hdr))) { err = "Writing the playable copy failed (disk full?)."; return abandon(ProxyResult::Error); }
    uint64_t frames = 0;
    std::vector<int16_t> buf;
    while (!r.eof()) {
        if (cancel.load(std::memory_order_acquire)) return abandon(ProxyResult::Cancelled);
        buf.clear();
        if (!r.readPcm16(buf, rate, err)) return abandon(ProxyResult::Error);   // ~1 s per block
        if (buf.empty()) continue;
        if (!writeAll(h, buf.data(), buf.size() * sizeof(int16_t))) {
            err = "Writing the playable copy failed (disk full?).";
            return abandon(ProxyResult::Error);
        }
        frames += buf.size() / ch;
    }
    const uint64_t dataBytes = frames * ch * 2;
    const uint64_t limit = g_rf64ThresholdForTest ? g_rf64ThresholdForTest : (0xFFFFFFFFull - 72);
    const bool rf64 = dataBytes > limit;
    wavHeader(hdr, rf64, rate, ch, dataBytes, frames);
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(h, zero, nullptr, FILE_BEGIN) || !writeAll(h, hdr, sizeof(hdr))) {
        err = "Writing the playable copy failed.";
        return abandon(ProxyResult::Error);
    }
    ::CloseHandle(h);
    // The house AV dance: a rename right after the write can sit in the on-write scan.
    bool landed = false;
    for (int t = 0; t < 5 && !landed; ++t) {
        if (::MoveFileExW(tmp.c_str(), out.c_str(), MOVEFILE_REPLACE_EXISTING)) landed = true;
        else ::Sleep(20);
    }
    if (!landed) {
        const DWORD e = ::GetLastError();
        deleteWithRetry(tmp);
        err = "The playable copy could not be put in place (" + std::to_string(e) + ").";
        return ProxyResult::Error;
    }
    if (facts) { facts->frames = frames; facts->rate = rate; facts->channels = ch; facts->rf64 = rf64; }
    return ProxyResult::Ok;
}

} // namespace pbt
