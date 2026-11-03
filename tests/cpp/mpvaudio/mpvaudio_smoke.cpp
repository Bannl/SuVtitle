// =============================================================================
// SuVtitle tests/cpp/mpvaudio/mpvaudio_smoke.cpp -- N3 (26/10/2026): the analysis' sound through libmpv
// (engine/TrMpvAudio.cpp + the libmpv path of engine/TrMedia.cpp) against the REAL libmpv-2.dll (SUVTITLE_MPV_DIR).
//
// THE ABI FIRST (compile time): TrMpvAudio.cpp declares the part of libmpv's C API it uses by itself (the engine builds with no
// mpv file). This file includes the REAL mpv/client.h, then TrMpvAudio.cpp, and static_asserts every size, offset, value and
// function signature of those declarations against it.
//
// Then, headless (no window, no sound device), each section a fresh reader:
//   [LOAD]   libmpv found where the engine looks (SUVTITLE_MPV_DIR here), loaded, no reason given
//   [TIME]   THE PLAYER'S TIMELINE: clips whose sound sits late / early against the picture (an MP4 edit list, MPEG-TS, a
//            Matroska track from 1.5 s, a file starting at 10 s, FLV, AAC priming) -> the bursts at the PLAYER's times,
//            +-2 ms (the table in make_media.py); Media Foundation's answers printed beside (FLV: it cannot open it)
//   [EOF]    the stream ends where the sound ends, eof() then, a read after it appends nothing
//   [TRACK]  ord 0 / 1 = 440 / 1320 Hz (MKV and MP4); "" = mpv's own pick (the track flagged default: what the player plays
//            with no pick); an ord the file lacks = mpv's refusal, FINAL (Media Foundation is not asked: its sentence differs)
//   [SEEK]   far = a new session ("mpv") landing on the linear decode's very sample (integer lag 0, correlation >= 0.9: mpv's
//            resampler restarts on its own sub-sample grid, so the values are not bit-identical); near = "forward" /
//            "pending" / "none"; under 2 s = "start"; in the silent tail / past the end = "end" + eof(), never an error;
//            FLAC, AAC, MP3 (Media Foundation's MP3 seek was 9.4 s wrong: TrMedia.h)
//   [ABORT]  a seek aborted at once: false, lastSeekAborted, a read refused; the next seek works
//   [ERR]    not media / no sound track / missing file: one sentence each (libmpv's, the player's word)
//   [NET]    a LOCAL file never makes mpv connect anywhere (review RV-NH 2): a playlist renamed .mkv, an HLS manifest renamed
//            .mp4, an mpv EDL -- a listener on 127.0.0.1 counts the connections (0)
//   [ENV]    no vkfallback folder next to this libmpv: the children inherit the environment as it is (empty block)
//   [CHILD]  in child processes (each needs a FRESH process: libmpv loads once): no libmpv -> Media Foundation decodes (note
//            says why) and FLV fails with its sentence; [VKFB] a PC without a system loader gets the BUNDLED one (bound to
//            libmpv, the children's PATH extended) and a PC WITH one keeps its own; [NODRV] no Vulkan driver at all
//            (VK_LOADER_DRIVERS_DISABLE=*) still decodes; [DLL] after the libmpv load, a DLL planted in the current
//            directory is no longer loaded by bare name (review RV-NH 4)
// Run: build_mpvaudio_smoke.cmd. Exit 0 = every check passed (2 = not run). --bench <file>... = speed / CPU, libmpv vs Media
// Foundation (a measurement, no verdict).
// =============================================================================
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "mpv/client.h"                        // THE REAL HEADER (SUVTITLE_MPV_DIR\include)
#include "../../../engine/TrMpvAudio.cpp"      // the product's file, in this translation unit (its mpvabi:: is checked below)
#include "../../../engine/TrMedia.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------------
// The ABI (compile time)
// ---------------------------------------------------------------------------------------------------------------------
namespace A = pbt::mpvabi;
#define SAME_SIZE(T) static_assert(sizeof(::T) == sizeof(A::T) && alignof(::T) == alignof(A::T), #T ": size / alignment")
#define SAME_AT(T, f) static_assert(offsetof(::T, f) == offsetof(A::T, f), #T "." #f ": offset")
#define SAME_VAL(v) static_assert((int)::v == (int)A::v, #v ": value")
SAME_SIZE(mpv_format); SAME_SIZE(mpv_event_id); SAME_SIZE(mpv_end_file_reason);
SAME_SIZE(mpv_node); SAME_AT(mpv_node, u); SAME_AT(mpv_node, format);
static_assert(sizeof(((::mpv_node*)nullptr)->u) == sizeof(((A::mpv_node*)nullptr)->u), "mpv_node.u: size");
SAME_SIZE(mpv_node_list); SAME_AT(mpv_node_list, num); SAME_AT(mpv_node_list, values); SAME_AT(mpv_node_list, keys);
SAME_SIZE(mpv_event_log_message); SAME_AT(mpv_event_log_message, prefix); SAME_AT(mpv_event_log_message, level);
SAME_AT(mpv_event_log_message, text); SAME_AT(mpv_event_log_message, log_level);
SAME_SIZE(mpv_event_end_file); SAME_AT(mpv_event_end_file, reason); SAME_AT(mpv_event_end_file, error);
SAME_AT(mpv_event_end_file, playlist_entry_id); SAME_AT(mpv_event_end_file, playlist_insert_id);
SAME_AT(mpv_event_end_file, playlist_insert_num_entries);
SAME_SIZE(mpv_event); SAME_AT(mpv_event, event_id); SAME_AT(mpv_event, error); SAME_AT(mpv_event, reply_userdata);
SAME_AT(mpv_event, data);
SAME_VAL(MPV_FORMAT_NONE); SAME_VAL(MPV_FORMAT_STRING); SAME_VAL(MPV_FORMAT_OSD_STRING); SAME_VAL(MPV_FORMAT_FLAG);
SAME_VAL(MPV_FORMAT_INT64); SAME_VAL(MPV_FORMAT_DOUBLE); SAME_VAL(MPV_FORMAT_NODE); SAME_VAL(MPV_FORMAT_NODE_ARRAY);
SAME_VAL(MPV_FORMAT_NODE_MAP); SAME_VAL(MPV_FORMAT_BYTE_ARRAY);
SAME_VAL(MPV_EVENT_NONE); SAME_VAL(MPV_EVENT_SHUTDOWN); SAME_VAL(MPV_EVENT_LOG_MESSAGE); SAME_VAL(MPV_EVENT_START_FILE);
SAME_VAL(MPV_EVENT_END_FILE); SAME_VAL(MPV_EVENT_FILE_LOADED); SAME_VAL(MPV_EVENT_PLAYBACK_RESTART);
SAME_VAL(MPV_END_FILE_REASON_EOF); SAME_VAL(MPV_END_FILE_REASON_STOP); SAME_VAL(MPV_END_FILE_REASON_QUIT);
SAME_VAL(MPV_END_FILE_REASON_ERROR); SAME_VAL(MPV_END_FILE_REASON_REDIRECT);
SAME_VAL(MPV_ERROR_PROPERTY_UNAVAILABLE); SAME_VAL(MPV_ERROR_LOADING_FAILED); SAME_VAL(MPV_ERROR_AO_INIT_FAILED);
SAME_VAL(MPV_ERROR_NOTHING_TO_PLAY); SAME_VAL(MPV_ERROR_UNKNOWN_FORMAT);
static_assert((MPV_CLIENT_API_VERSION >> 16) == A::kApiMajor, "the client API major version");
// the function signatures: the real type with mpv's own types swapped for TrMpvAudio.cpp's must be the member's type
template <class T> struct Tr { using type = T; };
template <> struct Tr<::mpv_handle*> { using type = A::mpv_handle*; };
template <> struct Tr<::mpv_format> { using type = A::mpv_format; };
template <> struct Tr<::mpv_node*> { using type = A::mpv_node*; };
template <> struct Tr<::mpv_event*> { using type = A::mpv_event*; };
template <class R, class... P> struct Tr<R (*)(P...)> { using type = typename Tr<R>::type (*)(typename Tr<P>::type...); };
#define SAME_FN(real, member) static_assert(std::is_same_v<Tr<decltype(&::real)>::type, decltype(A::Api::member)>, #real ": signature")
SAME_FN(mpv_client_api_version, client_api_version); SAME_FN(mpv_error_string, error_string);
SAME_FN(mpv_free_node_contents, free_node_contents); SAME_FN(mpv_create, create); SAME_FN(mpv_initialize, initialize);
SAME_FN(mpv_terminate_destroy, terminate_destroy); SAME_FN(mpv_set_option_string, set_option_string);
SAME_FN(mpv_command_node, command_node); SAME_FN(mpv_get_property, get_property);
SAME_FN(mpv_set_property_async, set_property_async); SAME_FN(mpv_request_log_messages, request_log_messages);
SAME_FN(mpv_wait_event, wait_event); SAME_FN(mpv_set_wakeup_callback, set_wakeup_callback);

// ---------------------------------------------------------------------------------------------------------------------
// The harness
// ---------------------------------------------------------------------------------------------------------------------
using pbt::AnalysisDecoder;
using pbt::AudioTrack;
using pbt::MediaInfo;
using pbt::MediaReader;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s%s%s\n", name, extra.empty() ? "" : "  (", extra.empty() ? "" : (extra + ")").c_str()); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
    std::fflush(stdout);
}
static std::string f(const char* fm, ...) {
    char b[1200];
    va_list ap;
    va_start(ap, fm);
    vsnprintf(b, sizeof b, fm, ap);
    va_end(ap);
    return b;
}
static std::string n8(const std::wstring& w) { return pbt::u8(w); }
static std::wstring w16(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static double nowMs() {
    static LARGE_INTEGER fq{};
    if (!fq.QuadPart) ::QueryPerformanceFrequency(&fq);
    LARGE_INTEGER c;
    ::QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)fq.QuadPart;
}
static double cpuMs() {
    FILETIME a, b, k, u;
    ::GetProcessTimes(::GetCurrentProcess(), &a, &b, &k, &u);
    auto t = [](FILETIME x) { return (double)(((uint64_t)x.dwHighDateTime << 32) | x.dwLowDateTime) / 10000.0; };
    return t(k) + t(u);
}
static std::wstring g_media;                         // work\media (next to the exe)
static std::wstring M(const wchar_t* name) { return g_media + L"\\" + name; }

static AudioTrack track(int ord) {
    AudioTrack t;
    if (ord >= 0) { t.chosen = true; t.ord = (uint32_t)ord; }
    return t;
}
// the whole 16 kHz stream of a file through the SHIPPING reader, the decoder asked
static bool decodeAll(const std::wstring& path, const AudioTrack& tr, AnalysisDecoder dec, std::vector<float>& out,
                      MediaInfo& info, std::string& err) {
    pbt::setAnalysisDecoder(dec);
    MediaReader r;
    out.clear();
    if (!r.open(path, MediaReader::Out::Mono16k, tr, err)) return false;
    info = r.info();
    int stall = 0;
    while (!r.eof()) {
        const size_t was = out.size();
        if (!r.readFloat(out, 16000 * 4, err)) return false;
        if (out.size() == was && ++stall > 100) { err = "no progress"; return false; }
    }
    return true;
}
// burst onsets (|x| over thr after a quiet stretch), in seconds
static std::vector<double> onsets(const std::vector<float>& x, float thr = 0.2f) {
    std::vector<double> o;
    for (size_t i = 0; i < x.size(); ++i) {
        if (std::fabs(x[i]) > thr) {
            o.push_back((double)i / 16000.0);
            i += 8000;                                   // skip the burst (0.5 s)
        }
    }
    return o;
}
static std::string list(const std::vector<double>& v) {
    std::string s;
    for (double d : v) s += f("%s%.4f", s.empty() ? "" : " ", d);
    return s.empty() ? "(none)" : s;
}
// exactly two bursts, at a and b (+-2 ms)
static bool burstsAt(const std::vector<float>& x, double a, double b) {
    const std::vector<double> on = onsets(x);
    return on.size() == 2 && std::fabs(on[0] - a) <= 0.002 && std::fabs(on[1] - b) <= 0.002;
}
// a steady tone's frequency from its zero crossings
static double freqOf(const std::vector<float>& x, size_t from, size_t n) {
    size_t z = 0;
    for (size_t i = from + 1; i < from + n && i < x.size(); ++i)
        if ((x[i - 1] < 0.0f) != (x[i] < 0.0f)) ++z;
    return (double)z * 16000.0 / 2.0 / (double)n;
}
// the integer lag (b against a) that maximises their correlation over a window, and that correlation
static void bestLag(const std::vector<float>& a, size_t aFrom, const std::vector<float>& b, size_t bFrom, size_t n,
                    int maxLag, int& lag, double& corr) {
    lag = 0;
    corr = -2.0;
    for (int L = -maxLag; L <= maxLag; ++L) {
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < n; ++i) {
            const int64_t ia = (int64_t)(aFrom + i), ib = (int64_t)(bFrom + i) + L;
            if (ia < 0 || ib < 0 || (size_t)ia >= a.size() || (size_t)ib >= b.size()) continue;
            sab += (double)a[(size_t)ia] * b[(size_t)ib];
            saa += (double)a[(size_t)ia] * a[(size_t)ia];
            sbb += (double)b[(size_t)ib] * b[(size_t)ib];
        }
        const double c = (saa > 0 && sbb > 0) ? sab / std::sqrt(saa * sbb) : -1.0;
        if (c > corr) { corr = c; lag = L; }
    }
}

// A child run of this very exe (a fresh process: libmpv loads once per process): its output, "RESULT key=value" lines.
static std::string runChild(const std::wstring& args, const std::wstring& cwd, const wchar_t* extraEnv = nullptr) {
    wchar_t exe[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    std::string out;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return out;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + args;
    // the child's environment: ours (+ one NAME=VALUE), as CreateProcess wants it (sorted: ours is, the extra one goes in place)
    std::vector<wchar_t> env;
    if (extraEnv) {
        std::vector<std::wstring> vars;
        LPWCH e = ::GetEnvironmentStringsW();
        for (const wchar_t* p = e; *p; p += wcslen(p) + 1) vars.emplace_back(p);
        ::FreeEnvironmentStringsW(e);
        const std::wstring add = extraEnv;
        auto at = std::find_if(vars.begin(), vars.end(), [&](const std::wstring& v) {
            return v[0] != L'=' && ::CompareStringOrdinal(v.c_str(), -1, add.c_str(), -1, TRUE) == CSTR_GREATER_THAN;
        });
        vars.insert(at, add);
        for (const std::wstring& v : vars) env.insert(env.end(), v.c_str(), v.c_str() + v.size() + 1);
        env.push_back(L'\0');
    }
    if (::CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (extraEnv ? CREATE_UNICODE_ENVIRONMENT : 0),
                         extraEnv ? env.data() : nullptr, cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
        ::CloseHandle(wr);
        wr = nullptr;
        char b[1024];
        DWORD n = 0;
        while (::ReadFile(rd, b, sizeof(b), &n, nullptr) && n) out.append(b, n);
        if (::WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 9);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
    }
    if (wr) ::CloseHandle(wr);
    ::CloseHandle(rd);
    return out;
}
static std::string resultOf(const std::string& out, const std::string& key) {
    const std::string k = "RESULT " + key + "=";
    const size_t p = out.find(k);
    if (p == std::string::npos) return "?";
    const size_t e = out.find_first_of("\r\n", p);
    return out.substr(p + k.size(), e == std::string::npos ? std::string::npos : e - p - k.size());
}

static std::wstring exeDirW() {
    wchar_t p[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring d(p, n);
    return d.substr(0, d.find_last_of(L"\\/"));
}
static bool exists(const std::wstring& p) { return ::GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static std::wstring envW(const wchar_t* n) {
    wchar_t b[2048] = {};
    const DWORD k = ::GetEnvironmentVariableW(n, b, 2048);
    return (k > 0 && k < 2048) ? std::wstring(b, k) : std::wstring();
}
static std::wstring moduleOf(const wchar_t* name) {
    HMODULE h = ::GetModuleHandleW(name);
    if (!h) return L"(not loaded)";
    wchar_t p[MAX_PATH] = {};
    ::GetModuleFileNameW(h, p, MAX_PATH);
    return p;
}
static std::wstring sys32() {
    wchar_t s[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(s, MAX_PATH);
    return std::wstring(s, n);
}
static bool ieq(const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

// ---------------------------------------------------------------------------------------------------------------------
// The children (each a fresh process)
// ---------------------------------------------------------------------------------------------------------------------
static int childMain(int argc, wchar_t** argv) {
    if (argc < 3) return 2;
    const std::wstring what = argv[2];
    g_media = exeDirW() + L"\\media";
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (what == L"nolib") {                          // no libmpv: Media Foundation decodes
        pbt::setMpvAudioDllDirForTest(exeDirW() + L"\\no_such_folder");
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const bool o = decodeAll(M(L"aud.m4a"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        std::printf("RESULT m4a_ok=%d\nRESULT m4a_decoder=%s\nRESULT m4a_note=%s\nRESULT m4a_onsets=%s\nRESULT m4a_at=%d\n", o ? 1 : 0,
                    mi.decoder.c_str(), mi.decoderNote.c_str(), list(onsets(x)).c_str(), burstsAt(x, 2.0, 5.0) ? 1 : 0);
        const bool o2 = decodeAll(M(L"late_flv.flv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        std::printf("RESULT flv_ok=%d\nRESULT flv_err=%s\n", o2 ? 1 : 0, err.c_str());
        return 0;
    }
    if (what == L"vkfb") {                            // argv[3] = the mpv folder with vkfallback\, argv[4] = 0 pretend absent / -1 real
        if (argc < 5) return 2;
        pbt::setMpvAudioDllDirForTest(argv[3]);
        pbt::setSystemVulkanLoaderForTest(_wtoi(argv[4]));
        std::string why;
        const bool avail = pbt::mpvAudioAvailable(why);
        std::printf("RESULT avail=%d\nRESULT why=%s\nRESULT vulkan=%s\n", avail ? 1 : 0, why.c_str(), n8(moduleOf(L"vulkan-1.dll")).c_str());
        const std::vector<wchar_t> env = pbt::environmentWithVulkanFallback();
        std::wstring path;
        for (const wchar_t* p = env.empty() ? L"" : env.data(); *p; p += wcslen(p) + 1)
            if (::CompareStringOrdinal(p, 5, L"PATH=", 5, TRUE) == CSTR_EQUAL) path = p + 5;
        const size_t cut = path.find_last_of(L';');
        std::printf("RESULT path_last=%s\n", n8(cut == std::wstring::npos ? path : path.substr(cut + 1)).c_str());
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const bool o = decodeAll(M(L"late_mkv.mkv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        std::printf("RESULT dec_ok=%d\nRESULT dec_decoder=%s\nRESULT dec_onsets=%s\nRESULT dec_at=%d\nRESULT dec_err=%s\n", o ? 1 : 0,
                    mi.decoder.c_str(), list(onsets(x)).c_str(), (o && mi.decoder == "mpv" && burstsAt(x, 3.5, 6.5)) ? 1 : 0,
                    err.c_str());
        return 0;
    }
    if (what == L"dll") {                             // CWD = a folder with two planted DLLs (renamed copies of a system DLL)
        HMODULE before = ::LoadLibraryW(L"zzn3plant1.dll");
        std::printf("RESULT before=%s\n", before ? n8(moduleOf(L"zzn3plant1.dll")).c_str() : "NOT LOADED");
        std::string why;
        const bool avail = pbt::mpvAudioAvailable(why);
        HMODULE after = ::LoadLibraryW(L"zzn3plant2.dll");
        std::printf("RESULT avail=%d\nRESULT after=%s\n", avail ? 1 : 0, after ? n8(moduleOf(L"zzn3plant2.dll")).c_str() : "NOT LOADED");
        // and the session still decodes (its own DLLs are found where they live)
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const bool o = decodeAll(M(L"aud.m4a"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        std::printf("RESULT dec_ok=%d\nRESULT dec_err=%s\n", o ? 1 : 0, err.c_str());
        return 0;
    }
    std::printf("unknown child %ls\n", what.c_str());
    return 2;
}

// --bench <file>...: the whole 16 kHz decode, libmpv then Media Foundation, speed and CPU (this process: mpv's threads are in it)
static int benchMain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("%-58s %-5s %9s %8s %9s %8s %7s  %s\n", "file", "dec", "audio s", "wall s", "x real", "cpu s", "cpu %", "note");
    for (int i = 2; i < argc; ++i) {
        const std::wstring p = argv[i];
        std::wstring leaf = p.substr(p.find_last_of(L"\\/") + 1);
        if (leaf.size() > 56) leaf = leaf.substr(0, 53) + L"...";
        for (int k = 0; k < 2; ++k) {
            const AnalysisDecoder dec = k == 0 ? AnalysisDecoder::MpvFirst : AnalysisDecoder::MediaFoundation;
            std::vector<float> x;
            MediaInfo mi;
            std::string err;
            const double t0 = nowMs(), c0 = cpuMs();
            const bool o = decodeAll(p, AudioTrack(), dec, x, mi, err);
            const double wall = (nowMs() - t0) / 1000.0, cpu = (cpuMs() - c0) / 1000.0;
            const double secs = (double)x.size() / 16000.0;
            // (the name as UTF-8: %ls stops at the first character the console's code page lacks)
            std::printf("%-58s %-5s %9.1f %8.2f %9.0f %8.2f %7.0f  %s\n", n8(leaf).c_str(), k == 0 ? "mpv" : "MF", secs, wall,
                        wall > 0 ? secs / wall : 0.0, cpu, wall > 0 ? 100.0 * cpu / wall : 0.0,
                        o ? (mi.decoder + " " + mi.codec + f(" %u Hz %u ch", mi.nativeRate, mi.nativeChannels)).c_str()
                          : ("FAILED: " + err).c_str());
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// [NET]: a listener on 127.0.0.1 that counts connections
// ---------------------------------------------------------------------------------------------------------------------
struct Listener {
    SOCKET s = INVALID_SOCKET;
    int port = 0;
    std::atomic<int> hits{0};
    std::atomic<bool> stop{false};
    std::thread th;
    bool start() {
        WSADATA wd;
        if (::WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(s, (sockaddr*)&a, sizeof(a)) != 0 || ::listen(s, 8) != 0) return false;
        int len = sizeof(a);
        ::getsockname(s, (sockaddr*)&a, &len);
        port = ntohs(a.sin_port);
        th = std::thread([this] {
            while (!stop) {
                fd_set r;
                FD_ZERO(&r);
                FD_SET(s, &r);
                timeval tv{0, 100000};
                if (::select(0, &r, nullptr, nullptr, &tv) > 0) {
                    SOCKET c = ::accept(s, nullptr, nullptr);
                    if (c != INVALID_SOCKET) { ++hits; ::closesocket(c); }
                }
            }
        });
        return true;
    }
    ~Listener() {
        stop = true;
        if (th.joinable()) th.join();
        if (s != INVALID_SOCKET) ::closesocket(s);
        ::WSACleanup();
    }
};

// ---------------------------------------------------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"--child") return childMain(argc, argv);
    if (argc >= 3 && std::wstring(argv[1]) == L"--bench") return benchMain(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== SuVtitle mpvaudio smoke (N3): the analysis' sound through libmpv ===\n");
    g_media = exeDirW() + L"\\media";
    if (!exists(M(L"late_mkv.mkv"))) { std::printf("NOT RUN: no media in %ls (build_mpvaudio_smoke.cmd makes it)\n", g_media.c_str()); return 2; }

    // ---- [LOAD] ---------------------------------------------------------------------------------------------------------
    {
        std::string why;
        const bool avail = pbt::mpvAudioAvailable(why);
        ok("[LOAD] libmpv-2.dll found and loaded (client API 2.x, every export), no reason given", avail && why.empty(),
           avail ? n8(pbt::mpvAudioDllDir()) : why);
        if (!avail) { std::printf("NOT RUN: libmpv unusable\n"); return 2; }
    }

    // ---- [TIME] the player's timeline -------------------------------------------------------------------------------
    struct Clip { const wchar_t* f; double a, b; const char* what; };
    const Clip clips[] = {
        {L"late_mp4.mp4", 3.5, 6.5, "MP4, the sound's edit list starts with a 1.5 s empty edit (AAC)"},
        {L"late_ts.ts", 3.5, 6.5, "MPEG-TS, the sound 1.5 s after the picture (TS clock from 1.4 s, rebased)"},
        {L"late_mkv.mkv", 3.5, 6.5, "Matroska, the sound's blocks from 1.5 s (FLAC)"},
        {L"ofs10_mkv.mkv", 2.0, 5.0, "Matroska, every stream from 10 s (the player shows it from 0)"},
        {L"early_mp4.mp4", 2.0, 5.0, "MP4, the PICTURE starts 0.5 s late (AAC priming)"},
        {L"aud.m4a", 2.0, 5.0, "M4A, sound only (AAC priming in the edit list)"},
        {L"late_flv.flv", 3.5, 6.5, "FLV, the sound 1.5 s late (AAC 44.1 kHz) -- Media Foundation cannot open FLV"},
    };
    for (const Clip& c : clips) {
        std::vector<float> x, y;
        MediaInfo mi, mf;
        std::string err, e2;
        const bool o = decodeAll(M(c.f), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        const std::vector<double> on = onsets(x);
        const bool mfOk = decodeAll(M(c.f), AudioTrack(), AnalysisDecoder::MediaFoundation, y, mf, e2);
        const bool at = burstsAt(x, c.a, c.b);
        ok(f("[TIME] %ls: the bursts at the player's %.1f / %.1f s +-2 ms (%s)", c.f, c.a, c.b, c.what).c_str(),
           o && mi.decoder == "mpv" && at,
           f("libmpv: %s, first sample at %.6f s; Media Foundation: %s", o ? list(on).c_str() : err.c_str(), mi.leadSec,
             mfOk ? list(onsets(y)).c_str() : e2.c_str()));
    }

    // ---- [INFO] the facts the FILE job logs and uses (duration = its end, picture or not) --------------------------------
    {
        std::vector<float> x;
        MediaInfo a, b, c;
        std::string err;
        const bool o1 = decodeAll(M(L"late_mkv.mkv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, a, err);
        const bool o2 = decodeAll(M(L"aud.m4a"), AudioTrack(), AnalysisDecoder::MpvFirst, x, b, err);
        const bool o3 = decodeAll(M(L"late_flv.flv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, c, err);
        ok("[INFO] duration, picture, sound track facts: late_mkv 11.5 s with a picture, FLAC 48 kHz mono; aud.m4a no picture, AAC",
           o1 && o2 && o3 && a.durationMs == 11500 && a.hasVideo && a.codec == "FLAC" && a.nativeRate == 48000
             && a.nativeChannels == 1 && a.seekMode == "mpv" && a.decoder == "mpv" && a.decoderNote.empty() && !b.hasVideo
             && b.codec == "AAC" && b.durationMs >= 9900 && b.durationMs <= 10100 && c.hasVideo && c.codec == "AAC"
             && c.nativeRate == 44100,
           f("%llu ms %s %s %u Hz %u ch / %llu ms %s %s / flv %s %u Hz", (unsigned long long)a.durationMs, a.hasVideo ? "video" : "no video",
             a.codec.c_str(), a.nativeRate, a.nativeChannels, (unsigned long long)b.durationMs, b.hasVideo ? "video" : "no video",
             b.codec.c_str(), c.codec.c_str(), c.nativeRate));
    }

    // ---- [FMT] the sound's FORMAT changes mid-stream (a broadcast .ts: stereo 48k -> 5.1 48k -> stereo 44.1k) ----------
    // mpv re-creates its audio output on a decoded-format change; before 02/11 that re-opened the pipe and killed the session
    // at the change ("audio output initialization failed": 0 s of this clip). The chain's output is now pinned (TrMpvAudio.cpp).
    {
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const bool o = decodeAll(M(L"fmtchange.ts"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        const double secs = (double)x.size() / 16000.0;
        const double a = o ? freqOf(x, 2 * 16000, 8000) : 0, b = o ? freqOf(x, 6 * 16000, 8000) : 0, c = o ? freqOf(x, 10 * 16000, 8000) : 0;
        ok("[FMT] a .ts whose sound goes stereo 48 kHz -> 5.1 -> stereo 44.1 kHz decodes WHOLE (12 s: 440 / 880 / 1320 Hz)",
           o && mi.decoder == "mpv" && secs > 11.7 && secs < 12.5 && std::fabs(a - 440) < 13 && std::fabs(b - 880) < 26
             && std::fabs(c - 1320) < 40,
           o ? f("%.3f s, %.0f / %.0f / %.0f Hz", secs, a, b, c) : err);
    }

    // ---- [GAP] timestamp gaps and resets (02/11): the player's clock follows the stamps, mpv's sound is continuous --------
    // (make_media.py: part A has a burst at 2.0 s -- 2.0214 on this TS's clock, its AAC priming has no edit list --, part B
    //  one 1.0 s in (3.0 s for the reset); the TS parts are joined byte for byte)
    {
        struct G { const wchar_t* f; double a, b; const char* what; };
        const G gs[] = {
            {L"gap_ts.ts", 2.0214, 8.0, "a ~2 s gap (B stamped +7 s): filled with silence, the burst after it at ITS place"},
            {L"gap_mkv.mkv", 2.0214, 8.0, "the same remuxed to Matroska"},
            {L"reset_ts.ts", 2.0214, 8.0561, "a timestamp RESET (B from 0 again): counted on, nothing dropped (Media Foundation's rule)"},
            {L"gap60.ts", 2.0214, 6.0561, "a 55 s jump: a clock reset (> 30 s), counted on, never 55 s of silence"},
        };
        for (const G& g : gs) {
            std::vector<float> x;
            MediaInfo mi;
            std::string err;
            const bool o = decodeAll(M(g.f), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
            const std::vector<double> on = onsets(x);
            const bool at = on.size() == 2 && std::fabs(on[0] - g.a) <= 0.002 && std::fabs(on[1] - g.b) <= 0.003;
            ok(f("[GAP] %ls: %s", g.f, g.what).c_str(), o && mi.decoder == "mpv" && at,
               o ? f("%s (%.3f s decoded)", list(on).c_str(), (double)x.size() / 16000.0) : err);
        }
        // 60 s after a reset, every frame RE-STAMPED by the chain: no drift (a coarse time base would make aresample trim)
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const bool o = decodeAll(M(L"reset_long.ts"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
        const std::vector<double> on = onsets(x, 0.3f);
        bool even = on.size() == 7;
        for (size_t k = 2; even && k < on.size(); ++k) even = std::fabs(on[k] - on[k - 1] - 10.0) <= 0.002;
        ok("[GAP] reset_long.ts: 60 s counted on after a reset, re-stamped frame by frame: the bursts exactly 10 s apart, none lost",
           o && even && x.size() > (size_t)(65.0 * 16000), o ? f("%s (%.3f s)", list(on).c_str(), (double)x.size() / 16000.0) : err);
    }

    // ---- [EOF] ----------------------------------------------------------------------------------------------------------
    {
        pbt::setAnalysisDecoder(AnalysisDecoder::MpvFirst);
        MediaReader r;
        std::string err;
        std::vector<float> x;
        const bool o = r.open(M(L"late_mkv.mkv"), MediaReader::Out::Mono16k, AudioTrack(), err);
        while (o && !r.eof() && r.readFloat(x, 16000 * 4, err)) {}
        const size_t n = x.size();
        std::vector<float> more;
        const bool again = r.readFloat(more, 16000, err);
        // 1.5 s of lead + the 10 s of sound = 11.5 s; the position counts it
        ok("[EOF] the stream ends where the sound ends (late_mkv: 1.5 s lead + 10 s), eof(), position() = what was handed out",
           o && r.eof() && n >= (size_t)(11.49 * 16000) && n <= (size_t)(11.51 * 16000) && r.position() == n,
           f("%.4f s", (double)n / 16000.0));
        ok("[EOF] a read after the end appends nothing and is not an error", again && more.empty(), err);
    }

    // ---- [TRACK] --------------------------------------------------------------------------------------------------------
    {
        struct T { const wchar_t* f; int ord; double hz; const char* how; };
        const T ts[] = {
            {L"two.mkv", 0, 440, "mpv"}, {L"two.mkv", 1, 1320, "mpv"}, {L"two.mkv", -1, 1320, "mpv-default"},
            {L"two.mp4", 0, 440, "mpv"}, {L"two.mp4", 1, 1320, "mpv"},
        };
        for (const T& t : ts) {
            std::vector<float> x;
            MediaInfo mi;
            std::string err;
            const bool o = decodeAll(M(t.f), track(t.ord), AnalysisDecoder::MpvFirst, x, mi, err);
            const double hz = o ? freqOf(x, 16000, 32000) : 0.0;
            ok(f("[TRACK] %ls ord %d -> the %.0f Hz track (%s)", t.f, t.ord, t.hz, t.ord < 0 ? "no pick: mpv's own, the default-flagged one"
                                                                                       : "the ord-th in container order").c_str(),
               o && std::fabs(hz - t.hz) < t.hz * 0.02 && mi.trackHow == t.how && mi.audioStreams == 2
                 && mi.trackOrd == (t.ord < 0 ? 1 : t.ord),
               o ? f("%.1f Hz, %s, ord %d, id %u, \"%s\" %s", hz, mi.trackHow.c_str(), mi.trackOrd, mi.trackId, mi.trackLang.c_str(),
                     mi.codec.c_str()) : err);
        }
        std::vector<float> x;
        MediaInfo mi;
        std::string err;
        const bool o = decodeAll(M(L"two.mkv"), track(2), AnalysisDecoder::MpvFirst, x, mi, err);
        ok("[TRACK] an ord the file lacks: refused by libmpv, FINAL (never another track; Media Foundation not asked)",
           !o && err == "The chosen sound track (number 3) is not in this file.", err);
    }

    // ---- [SEEK] ---------------------------------------------------------------------------------------------------------
    {
        const wchar_t* longs[] = {L"long.mkv", L"long.mp4", L"long.mp3"};
        for (const wchar_t* lf : longs) {
            std::vector<float> L;
            MediaInfo li;
            std::string err;
            if (!decodeAll(M(lf), AudioTrack(), AnalysisDecoder::MpvFirst, L, li, err)) { ok(f("[SEEK] %ls decodes", lf).c_str(), false, err); continue; }
            pbt::setAnalysisDecoder(AnalysisDecoder::MpvFirst);
            MediaReader r;
            r.open(M(lf), MediaReader::Out::Mono16k, AudioTrack(), err);
            // targets: far ahead / behind (a new session), on a burst at 13 / 53 / 103 s and 0.2 s before it
            const double targets[] = {52.8, 12.9, 102.95, 33.0};
            for (double t : targets) {
                const uint64_t T = (uint64_t)std::llround(t * 16000.0);
                const bool sk = r.seek(T, err);
                const std::string how = r.lastSeekHow();
                std::vector<float> got;
                while (sk && got.size() < 16000 && !r.eof()) if (!r.readFloat(got, 16000 - got.size(), err)) break;
                int lag = 99;
                double corr = 0;
                bestLag(L, (size_t)T, got, 0, std::min<size_t>(got.size(), 16000), 4, lag, corr);
                // each decode is within HALF a 16 kHz sample of the player's clock (its own resampler grid, rounded to a
                // frame), so the two agree within ONE sample: 62.5 us
                ok(f("[SEEK] %ls seek(%.2f s) = a new session on the linear decode's samples (|lag| <= 1, corr >= 0.9)", lf, t).c_str(),
                   sk && how == "mpv" && r.position() == T + got.size() && std::abs(lag) <= 1 && corr >= 0.9,
                   f("%s, lag %d, corr %.4f, %zu read", how.c_str(), lag, corr, got.size()));
            }
            // near: within what is decoded / a little ahead / where it is
            {
                uint64_t at = r.position();
                std::vector<float> got;
                r.seek(at + 1600, err);
                const std::string h1 = r.lastSeekHow();
                r.seek(r.position() + 16000 * 5, err);
                const std::string h2 = r.lastSeekHow();
                at = r.position();
                r.seek(at, err);
                const std::string h3 = r.lastSeekHow();
                r.readFloat(got, 1600, err);
                int lag = 99;
                double corr = 0;
                bestLag(L, (size_t)at, got, 0, got.size(), 4, lag, corr);
                ok(f("[SEEK] %ls near targets read forward (pending / forward / none), still on the linear samples", lf).c_str(),
                   (h1 == "pending" || h1 == "forward") && h2 == "forward" && h3 == "none" && lag == 0 && corr > 0.99,
                   f("%s / %s / %s, lag %d corr %.5f", h1.c_str(), h2.c_str(), h3.c_str(), lag, corr));
            }
            {
                const bool sk = r.seek(16000 * 1, err);
                ok(f("[SEEK] %ls under 2 s: \"start\"", lf).c_str(), sk && std::string(r.lastSeekHow()) == "start", r.lastSeekHow());
            }
        }
        // the end: the silent tail of a picture longer than its sound, and past everything
        {
            pbt::setAnalysisDecoder(AnalysisDecoder::MpvFirst);
            MediaReader r;
            std::string err;
            r.open(M(L"shortaud.mkv"), MediaReader::Out::Mono16k, AudioTrack(), err);
            const bool s2 = r.seek(16000 * 100, err);                    // past everything (a new session: > 10 s ahead)
            const std::string h2 = r.lastSeekHow();
            const bool e2 = r.eof();
            const bool s1 = r.seek(16000 * 7, err);                      // behind: a new session, in the sound's silent tail
            const std::string h1 = r.lastSeekHow();
            const bool e1 = r.eof();
            std::vector<float> x;
            const bool rd1 = r.readFloat(x, 16000, err);
            const bool s3 = r.seek((uint64_t)(4.5 * 16000), err);         // back inside the sound: its last 0.5 s
            const std::string h3 = r.lastSeekHow();
            std::vector<float> y;
            while (s3 && !r.eof() && r.readFloat(y, 16000, err)) {}
            ok("[SEEK] past the end / in the sound's silent tail: \"end\", eof(), no error; back inside: the last 0.5 s",
               s2 && h2 == "end" && e2 && s1 && h1 == "end" && e1 && rd1 && x.empty() && s3 && h3 == "mpv"
                 && y.size() >= 7900 && y.size() <= 8100,
               f("%s eof=%d / %s eof=%d / %s %zu frames after 4.5 s %s", h2.c_str(), e2 ? 1 : 0, h1.c_str(), e1 ? 1 : 0, h3.c_str(),
                 y.size(), err.c_str()));
        }
    }

    // ---- [ABORT] --------------------------------------------------------------------------------------------------------
    {
        pbt::setAnalysisDecoder(AnalysisDecoder::MpvFirst);
        MediaReader r;
        std::string err;
        r.open(M(L"long.mkv"), MediaReader::Out::Mono16k, AudioTrack(), err);
        const std::function<bool()> stop = [] { return true; };
        const bool sk = r.seek(16000 * 60, err, stop);
        const bool aborted = r.lastSeekAborted();
        std::vector<float> x;
        std::string e2;
        const bool rd = r.readFloat(x, 1600, e2);
        const bool sk2 = r.seek(16000 * 60, err);
        std::vector<float> y;
        const bool rd2 = r.readFloat(y, 1600, err);
        ok("[ABORT] a seek aborted at once: false + lastSeekAborted, a read refused, the next seek lands",
           !sk && aborted && !rd && sk2 && !r.lastSeekAborted() && rd2 && y.size() == 1600 && r.position() == 16000 * 60 + 1600,
           f("read: %s", e2.c_str()));
    }

    // ---- [ERR] ----------------------------------------------------------------------------------------------------------
    {
        std::vector<float> x;
        MediaInfo mi;
        std::string e1, e2, e3;
        const bool a = decodeAll(M(L"notmedia.mkv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, e1);
        const bool b = decodeAll(M(L"vonly.mkv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, e2);
        const bool c = decodeAll(M(L"no_such_file.mkv"), AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, e3);
        ok("[ERR] not media: libmpv's sentence (Media Foundation could not read it either)",
           !a && e1 == "SuVtitle cannot read this file's format.", e1);
        ok("[ERR] a picture with no sound track: \"This file has no audio track.\"", !b && e2 == "This file has no audio track.", e2);
        ok("[ERR] a missing file: libmpv's sentence", !c && e3.find("could not be opened") != std::string::npos, e3);
    }

    // ---- [NET] a local file never makes mpv connect (review RV-NH 2) -------------------------------------------------------
    {
        Listener L;
        if (!L.start()) {
            ok("[NET] a listener on 127.0.0.1", false, "socket");
        } else {
            const std::wstring dir = exeDirW() + L"\\net";
            ::CreateDirectoryW(dir.c_str(), nullptr);
            struct C { const wchar_t* f; std::string body; };
            const C cs[] = {
                {L"m3u.mkv", f("#EXTM3U\n#EXTINF:10,x\nhttp://127.0.0.1:%d/from_m3u.mkv\n", L.port)},
                {L"hls.mp4", f("#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXTINF:10.0,\nhttp://127.0.0.1:%d/from_hls.ts\n#EXT-X-ENDLIST\n", L.port)},
                {L"edl.mkv", f("# mpv EDL v0\nhttp://127.0.0.1:%d/from_edl.mkv,0,5\n", L.port)},
                {L"m3u8.mp3", f("#EXTM3U\nhttp://127.0.0.1:%d/from_m3u8.mp3\n", L.port)},
            };
            for (const C& c : cs) {
                const std::wstring p = dir + L"\\" + c.f;
                HANDLE h = ::CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                DWORD w = 0;
                ::WriteFile(h, c.body.data(), (DWORD)c.body.size(), &w, nullptr);
                ::CloseHandle(h);
                const int before = L.hits;
                std::vector<float> x;
                MediaInfo mi;
                std::string err;
                const bool o = decodeAll(p, AudioTrack(), AnalysisDecoder::MpvFirst, x, mi, err);
                ::Sleep(1500);                           // a late connection would land here
                ok(f("[NET] %ls (a local file naming http://127.0.0.1): no connection", c.f).c_str(), L.hits == before,
                   f("%d connection(s); the reader: %s", L.hits - before, o ? "opened" : err.c_str()));
            }
        }
    }

    // ---- [ENV] ------------------------------------------------------------------------------------------------------------
    {
        const bool noFb = pbt::vulkanFallbackDir().empty();
        ok("[ENV] no vkfallback\\ next to this libmpv: the children inherit the environment as it is (empty block)",
           !noFb || pbt::environmentWithVulkanFallback().empty(), noFb ? "no fallback folder" : n8(pbt::vulkanFallbackDir()));
    }

    // ---- [CHILD] each in a fresh process ---------------------------------------------------------------------------------
    {
        const std::string o = runChild(L"--child nolib", std::wstring());
        ok("[CHILD nolib] no libmpv: Media Foundation decodes the M4A, and says why libmpv did not",
           resultOf(o, "m4a_ok") == "1" && resultOf(o, "m4a_decoder") == "mf"
             && resultOf(o, "m4a_note").find("libmpv unavailable") == 0 && resultOf(o, "m4a_at") == "1",
           resultOf(o, "m4a_note") + " | onsets " + resultOf(o, "m4a_onsets"));
        ok("[CHILD nolib] ...and the FLV, which only libmpv reads, fails with Media Foundation's sentence",
           resultOf(o, "flv_ok") == "0" && resultOf(o, "flv_err").find("Windows cannot read") == 0, resultOf(o, "flv_err"));
    }
    {
        // the bundle's layout in work\vkfb: libmpv-2.dll (a hard link to SUVTITLE_MPV_DIR's, else a copy) + vkfallback\vulkan-1.dll
        const std::wstring rt = envW(L"SUVTITLE_VULKAN_RT");
        const std::wstring vkSrc = rt + L"\\x64\\vulkan-1.dll";
        if (rt.empty() || !exists(vkSrc)) {
            std::printf("  NOT CHECKED  [CHILD vkfb] [CHILD nodrv]: set SUVTITLE_VULKAN_RT to the extracted VulkanRT Components folder\n");
        } else {
            const std::wstring lay = exeDirW() + L"\\vkfb", fbDir = lay + L"\\vkfallback";
            ::CreateDirectoryW(lay.c_str(), nullptr);
            ::CreateDirectoryW(fbDir.c_str(), nullptr);
            const std::wstring dll = lay + L"\\libmpv-2.dll";
            if (!exists(dll) && !::CreateHardLinkW(dll.c_str(), (pbt::mpvAudioDllDir() + L"\\libmpv-2.dll").c_str(), nullptr))
                ::CopyFileW((pbt::mpvAudioDllDir() + L"\\libmpv-2.dll").c_str(), dll.c_str(), FALSE);
            ::CopyFileW(vkSrc.c_str(), (fbDir + L"\\vulkan-1.dll").c_str(), FALSE);
            const std::wstring want = fbDir + L"\\vulkan-1.dll";
            const std::string a = runChild(L"--child vkfb \"" + lay + L"\" 0", std::wstring());
            ok("[CHILD vkfb] System32 without a loader (simulated): the BUNDLED one is loaded by full path and libmpv binds to it",
               resultOf(a, "avail") == "1" && ieq(w16(resultOf(a, "vulkan")), want), resultOf(a, "vulkan") + " " + resultOf(a, "why"));
            ok("[CHILD vkfb] ...the children's PATH ends with vkfallback\\, and the sound decodes",
               ieq(w16(resultOf(a, "path_last")), fbDir) && resultOf(a, "dec_ok") == "1" && resultOf(a, "dec_at") == "1",
               resultOf(a, "path_last") + " | " + resultOf(a, "dec_onsets") + " " + resultOf(a, "dec_err"));
            const std::string b = runChild(L"--child vkfb \"" + lay + L"\" -1", std::wstring());
            ok("[CHILD vkfb] a PC WITH a system loader keeps its own (System32), the bundled one beside libmpv notwithstanding",
               resultOf(b, "avail") == "1" && ieq(w16(resultOf(b, "vulkan")), sys32() + L"\\vulkan-1.dll") && resultOf(b, "dec_ok") == "1",
               resultOf(b, "vulkan"));
            const std::string c = runChild(L"--child vkfb \"" + lay + L"\" 0", std::wstring(), L"VK_LOADER_DRIVERS_DISABLE=*");
            ok("[CHILD nodrv] the bundled loader with NO driver behind it (VK_LOADER_DRIVERS_DISABLE=*): libmpv loads, the sound decodes",
               resultOf(c, "avail") == "1" && ieq(w16(resultOf(c, "vulkan")), want) && resultOf(c, "dec_ok") == "1"
                 && resultOf(c, "dec_at") == "1",
               resultOf(c, "vulkan") + " | " + resultOf(c, "dec_onsets") + " " + resultOf(c, "dec_err"));
        }
    }
    {
        const std::wstring plant = exeDirW() + L"\\plant";
        ::CreateDirectoryW(plant.c_str(), nullptr);
        const std::wstring src = sys32() + L"\\msimg32.dll";       // a tiny, harmless system DLL stands for a planted one
        ::CopyFileW(src.c_str(), (plant + L"\\zzn3plant1.dll").c_str(), FALSE);
        ::CopyFileW(src.c_str(), (plant + L"\\zzn3plant2.dll").c_str(), FALSE);
        const std::string o = runChild(L"--child dll", plant);
        ok("[CHILD dll] control: before libmpv, a DLL planted in the current directory DOES load by bare name (the plant works)",
           ieq(w16(resultOf(o, "before")), plant + L"\\zzn3plant1.dll"), resultOf(o, "before"));
        ok("[CHILD dll] after the libmpv load, the current directory is off the DLL search path (RV-NH 4), the sound still decodes",
           resultOf(o, "avail") == "1" && resultOf(o, "after") == "NOT LOADED" && resultOf(o, "dec_ok") == "1",
           resultOf(o, "after") + " | " + resultOf(o, "dec_err"));
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    std::printf(g_fail ? "MPVAUDIO SMOKE FAILED\n" : "MPVAUDIO SMOKE PASSED\n");
    return g_fail ? 1 : 0;
}
