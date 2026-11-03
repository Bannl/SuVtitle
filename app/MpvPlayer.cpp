// =============================================================================
// SuVtitle - app/MpvPlayer.cpp   (the native video engine: libmpv under the page)
//
// N1 (26/10/2026), PORTED from the N0 prototype (scratchpad\n0\src\n0proto.cpp -- PASSED on the user's own screen 26/10:
// Hilda HEVC Main 10 and Dragon Ball in full screen, pause / play many times, no freeze, hwdec=d3d11va, 0 drops): the
// engine's half of it -- libmpv loaded at run time, the DISABLED `wid` child, the options, the wakeup -> post -> drain on
// the UI thread, the termination that never blocks the UI thread on mpv's cross-thread window, "setting B jumps to A".
// What the product adds: item GENERATIONS (an event says which item it belongs to: loadfile's playlist_entry_id), the
// page's CSS rectangle -> pixels, full screen's interim placement, the sound track by `ord`, the LINEAR volume of
// <video>, WARP detected BEFORE the first file (profile=fast: N0 measured 900-1200 % of a core without it), the 30 Hz
// clock, and native.log (what mpv said, for the PC we cannot see).
// FIX ROUND (26/10 evening, docs\REVIEW_RV-NH.md + REVIEW_RV-NP.md L1, each measured on this libmpv by the reviewer):
//   HIGH1 a runtime track switch (a GLOBAL aid) carried over to the next item opened with ord -1 -> aid is ALWAYS file-local
//         (aid=auto when no track is asked);
//   HIGH2 a local file whose content is a playlist / HLS manifest made mpv fetch its URLs -> access-references=no;
//   MEDIUM3 a seek asked during a load was refused by mpv and lost -> kept (last wins) and done at the item's file-loaded;
//   L1    play() at eof restarted from 0 over a seek still on its way -> only when nothing was sought since that eof;
//   LOW7  the GPU probe's thread could write into a freed Impl -> it owns its own (shared) result block;
//   LOW8  the first open() waited up to 3 s for the probe -> never waits (profile=fast applied when the answer comes);
//   LOW9  native.log grew past its cap within a session, and a failed load() never reached it -> capped while it writes,
//         opened by load();
//   MEDIUM4 the current directory stayed on the DLL search path -> lockDllSearch() (wWinMain's first call);
//   + N3's Vulkan loader fallback (preloadVulkanFallback: System32's first, else <dllDir>\vkfallback\ by full path);
//   + the contract's Tracks / audioList / hasVideo: track-list read AT file-loaded -- asked ASYNCHRONOUSLY (the brief said
//     "synchronously"; mpv makes its video window, a cross-thread child of ours, right after file-loaded: this thread never
//     waits on mpv's core there) -- and Tracks then Loaded told with the answer, so both are known at Loaded.
// UI THREAD ONLY (see MpvPlayer.h). ASCII only (no /utf-8 in this build).
// =============================================================================
#include "MpvPlayer.h"

#include <d3d11.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace suvmpv {

// ---------------------------------------------------------------------------------------------------------------------
// libmpv's C API: the subset SuVtitle uses, declared HERE (mpv/client.h, client API 2.x, ISC licence) so the build never
// needs mpv's headers -- the DLL is optional. tests/cpp/mpv/mpv_smoke.cpp compiles this file next to the REAL client.h and
// static_asserts every size, offset and value below against it: an ABI drift fails the smoke, never a user.
// ---------------------------------------------------------------------------------------------------------------------
namespace abi {

struct mpv_handle;   // opaque

enum mpv_format : int {
    MPV_FORMAT_NONE = 0, MPV_FORMAT_STRING = 1, MPV_FORMAT_OSD_STRING = 2, MPV_FORMAT_FLAG = 3, MPV_FORMAT_INT64 = 4,
    MPV_FORMAT_DOUBLE = 5, MPV_FORMAT_NODE = 6, MPV_FORMAT_NODE_ARRAY = 7, MPV_FORMAT_NODE_MAP = 8, MPV_FORMAT_BYTE_ARRAY = 9
};
enum mpv_event_id : int {
    MPV_EVENT_NONE = 0, MPV_EVENT_SHUTDOWN = 1, MPV_EVENT_LOG_MESSAGE = 2, MPV_EVENT_GET_PROPERTY_REPLY = 3,
    MPV_EVENT_SET_PROPERTY_REPLY = 4, MPV_EVENT_COMMAND_REPLY = 5, MPV_EVENT_START_FILE = 6, MPV_EVENT_END_FILE = 7,
    MPV_EVENT_FILE_LOADED = 8, MPV_EVENT_IDLE = 11, MPV_EVENT_TICK = 14, MPV_EVENT_CLIENT_MESSAGE = 16,
    MPV_EVENT_VIDEO_RECONFIG = 17, MPV_EVENT_AUDIO_RECONFIG = 18, MPV_EVENT_SEEK = 20, MPV_EVENT_PLAYBACK_RESTART = 21,
    MPV_EVENT_PROPERTY_CHANGE = 22, MPV_EVENT_QUEUE_OVERFLOW = 24, MPV_EVENT_HOOK = 25
};
enum mpv_log_level : int {
    MPV_LOG_LEVEL_NONE = 0, MPV_LOG_LEVEL_FATAL = 10, MPV_LOG_LEVEL_ERROR = 20, MPV_LOG_LEVEL_WARN = 30,
    MPV_LOG_LEVEL_INFO = 40, MPV_LOG_LEVEL_V = 50, MPV_LOG_LEVEL_DEBUG = 60, MPV_LOG_LEVEL_TRACE = 70
};
enum mpv_end_file_reason : int {
    MPV_END_FILE_REASON_EOF = 0, MPV_END_FILE_REASON_STOP = 2, MPV_END_FILE_REASON_QUIT = 3,
    MPV_END_FILE_REASON_ERROR = 4, MPV_END_FILE_REASON_REDIRECT = 5
};
enum mpv_error : int {
    MPV_ERROR_LOADING_FAILED = -13, MPV_ERROR_AO_INIT_FAILED = -14, MPV_ERROR_VO_INIT_FAILED = -15,
    MPV_ERROR_NOTHING_TO_PLAY = -16, MPV_ERROR_UNKNOWN_FORMAT = -17, MPV_ERROR_UNSUPPORTED = -18
};

struct mpv_node_list;
struct mpv_byte_array;
struct mpv_node {
    union {
        char* string;
        int flag;
        int64_t int64;
        double double_;
        mpv_node_list* list;
        mpv_byte_array* ba;
    } u;
    mpv_format format;
};
struct mpv_node_list {
    int num;
    mpv_node* values;
    char** keys;
};
struct mpv_event_property {
    const char* name;
    mpv_format format;
    void* data;
};
struct mpv_event_log_message {
    const char* prefix;
    const char* level;
    const char* text;
    mpv_log_level log_level;
};
struct mpv_event_start_file {
    int64_t playlist_entry_id;
};
struct mpv_event_end_file {
    mpv_end_file_reason reason;
    int error;
    int64_t playlist_entry_id;
    int64_t playlist_insert_id;
    int playlist_insert_num_entries;
};
struct mpv_event_command {
    mpv_node result;
};
struct mpv_event {
    mpv_event_id event_id;
    int error;
    uint64_t reply_userdata;
    void* data;
};

constexpr unsigned long kApiMajor = 2;   // MPV_CLIENT_API_VERSION >> 16 of the headers these declarations mirror

// The functions, resolved from the DLL once per process (the DLL is never unloaded: mpv's threads may outlive a handle).
struct Api {
    HMODULE dll = nullptr;
    unsigned long (*client_api_version)() = nullptr;
    const char* (*error_string)(int) = nullptr;
    void (*free_mem)(void*) = nullptr;                                         // mpv_free
    mpv_handle* (*create)() = nullptr;
    int (*initialize)(mpv_handle*) = nullptr;
    void (*terminate_destroy)(mpv_handle*) = nullptr;
    int (*set_option)(mpv_handle*, const char*, mpv_format, void*) = nullptr;
    int (*set_option_string)(mpv_handle*, const char*, const char*) = nullptr;
    int (*command_async)(mpv_handle*, uint64_t, const char**) = nullptr;
    int (*command_node_async)(mpv_handle*, uint64_t, mpv_node*) = nullptr;
    int (*set_property_async)(mpv_handle*, uint64_t, const char*, mpv_format, void*) = nullptr;
    char* (*get_property_string)(mpv_handle*, const char*) = nullptr;
    int (*get_property_async)(mpv_handle*, uint64_t, const char*, mpv_format) = nullptr;   // (the track list, at file-loaded)
    int (*observe_property)(mpv_handle*, uint64_t, const char*, mpv_format) = nullptr;
    int (*request_log_messages)(mpv_handle*, const char*) = nullptr;
    mpv_event* (*wait_event)(mpv_handle*, double) = nullptr;
    void (*set_wakeup_callback)(mpv_handle*, void (*)(void*), void*) = nullptr;
};
Api g_api;

}   // namespace abi

namespace {

using namespace abi;

constexpr wchar_t kVideoClass[] = L"SuVtitle.Video";
constexpr UINT kWmWakeup   = WM_APP + 0x61;   // mpv has events (its wakeup callback, any thread)
constexpr UINT kWmProbe    = WM_APP + 0x62;   // the GPU probe finished (its thread)
constexpr UINT kWmMpvGone  = WM_APP + 0x63;   // mpv shut itself down: the handle is let go off the drain
constexpr UINT_PTR kTimerTime = 1;            // the trailing Time event of the 30 Hz clock
constexpr ULONGLONG kTimeEveryMs = 33;        // the contract's "at most 30 Hz while playing"
constexpr int kLogLinesPerMinute = 300;
constexpr ULONGLONG kLogCapBytes = 1024u * 1024u;   // native.log is started again past this -- checked while it writes
constexpr uint64_t kReplyLoad = 1ULL << 40;   // loadfile's reply_userdata: kReplyLoad | sequence
constexpr uint64_t kReplyCmd = 1ULL << 41;    // any other command's: kReplyCmd | sequence (a refusal is logged by name)
constexpr uint64_t kReplyTracks = 1ULL << 42; // the track list asked at file-loaded: kReplyTracks | the item's gen

// A string of the FILE's (a track's title, language, codec): untrusted -- control characters dropped, at most `cap` bytes,
// cut on a UTF-8 boundary (never half a character).
std::string fileText(const char* s, size_t cap) {
    std::string o;
    if (!s) return o;
    for (const char* p = s; *p && o.size() < cap; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7f) continue;
        o += static_cast<char>(c);
    }
    if (o.size() >= cap) {                      // (cut: the last character may be incomplete -- dropped if it is)
        const size_t n = o.size();
        size_t i = n;
        while (i > 0 && (static_cast<unsigned char>(o[i - 1]) & 0xC0) == 0x80) --i;   // o[i - 1] = the last lead byte
        if (i > 0) {
            const unsigned char lead = static_cast<unsigned char>(o[i - 1]);
            const size_t want = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
            if (n - (i - 1) < want) o.resize(i - 1);
        }
    }
    return o;
}

enum Prop : uint64_t {
    P_TIME = 1, P_DUR, P_PAUSE, P_EOF, P_SPEED, P_VOL, P_MUTE, P_ABA, P_ABB, P_DW, P_DH, P_HWDEC, P_TRACKS, P_SEEKING,
    P_SEEKABLE, P_IDLE, P_VO, P_VCODEC, P_ACODEC
};

std::string u8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::string fmt(const char* f, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

// milliseconds of a precise clock (the 30 Hz pacing: GetTickCount64 moves in 15.6 ms steps)
ULONGLONG nowMs() {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) ::QueryPerformanceFrequency(&f);
    LARGE_INTEGER c{};
    ::QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart / (f.QuadPart / 1000));
}

std::wstring exeDir() {
    wchar_t p[MAX_PATH] = {};
    const DWORD n = ::GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring d(p, n < MAX_PATH ? n : 0);
    const size_t s = d.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : d.substr(0, s);
}

// THE VULKAN LOADER (1.1.0, N3's patch -- engine/TrMpvAudio.h applies the same rule to the engine's libmpv). libmpv-2.dll
// imports vulkan-1.dll HARD: on a PC whose graphics driver never installed a Vulkan loader, libmpv would not even load. The
// bundle ships Khronos' loader (LunarG's VulkanRT redistributable) in <dllDir>\vkfallback\ -- its OWN folder, never next to
// libmpv nor in a folder given to AddDllDirectory (LOAD_LIBRARY_SEARCH_DEFAULT_DIRS searches those BEFORE System32: a copy
// there would beat the PC's own loader): a PC with a loader in System32 keeps its own. Loaded HERE by FULL PATH, before
// libmpv, ONLY when System32 has none; libmpv's import of "vulkan-1.dll" then binds to that module (a DLL of that name
// already in the process is used, wherever it came from). With no driver behind it the loader lists no device:
// gpu-api=d3d11 plays (measured, PROGRESS_N3). Independent of the process's default DLL search order (full path, flags).
std::string preloadVulkanFallback(const std::wstring& dllDir) {
    wchar_t sys[MAX_PATH] = {};
    const UINT n = ::GetSystemDirectoryW(sys, MAX_PATH);              // System32 (this process is 64-bit)
    if (n > 0 && n < MAX_PATH && ::GetFileAttributesW((std::wstring(sys, n) + L"\\vulkan-1.dll").c_str()) != INVALID_FILE_ATTRIBUTES)
        return "the system's (System32)";
    std::wstring d = dllDir;
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
    const std::wstring fb = d + L"\\vkfallback\\vulkan-1.dll";
    if (::GetFileAttributesW(fb.c_str()) == INVALID_FILE_ATTRIBUTES) return "none (no loader in System32, none bundled)";
    if (!::LoadLibraryExW(fb.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32))
        return fmt("the bundled one could not be loaded (Windows error %lu)", ::GetLastError());
    return "the bundled one (" + u8(fb) + ")";
}

template <class F>
bool sym(HMODULE m, F& f, const char* name, std::string& missing) {
    f = reinterpret_cast<F>(reinterpret_cast<void*>(::GetProcAddress(m, name)));
    if (!f && missing.empty()) missing = name;
    return f != nullptr;
}

double propDouble(const mpv_event_property& p, double def) {
    return (p.format == MPV_FORMAT_DOUBLE && p.data) ? *static_cast<const double*>(p.data) : def;
}
int64_t propInt(const mpv_event_property& p, int64_t def) {
    if (p.format == MPV_FORMAT_INT64 && p.data) return *static_cast<const int64_t*>(p.data);
    if (p.format == MPV_FORMAT_DOUBLE && p.data) return (int64_t)*static_cast<const double*>(p.data);
    return def;
}
bool propFlag(const mpv_event_property& p, bool def) {
    return (p.format == MPV_FORMAT_FLAG && p.data) ? (*static_cast<const int*>(p.data) != 0) : def;
}
std::string propStr(const mpv_event_property& p) {
    if (p.format == MPV_FORMAT_STRING && p.data) {
        const char* s = *static_cast<char* const*>(p.data);
        return s ? s : "";
    }
    return std::string();
}
const mpv_node* mapGet(const mpv_node& m, const char* key) {
    if (m.format != MPV_FORMAT_NODE_MAP || !m.u.list) return nullptr;
    const mpv_node_list* l = m.u.list;
    for (int i = 0; i < l->num; ++i)
        if (l->keys && l->keys[i] && std::strcmp(l->keys[i], key) == 0) return &l->values[i];
    return nullptr;
}
int64_t nodeInt(const mpv_node* n, int64_t def) {
    if (!n) return def;
    if (n->format == MPV_FORMAT_INT64) return n->u.int64;
    if (n->format == MPV_FORMAT_DOUBLE) return (int64_t)n->u.double_;
    return def;
}
bool nodeFlag(const mpv_node* n) { return n && n->format == MPV_FORMAT_FLAG && n->u.flag != 0; }
std::string nodeStr(const mpv_node* n) {
    return (n && n->format == MPV_FORMAT_STRING && n->u.string) ? std::string(n->u.string) : std::string();
}

// mpv's A-B loop points are "no" or a time in seconds
int64_t abMs(const std::string& v) {
    if (v.empty() || v == "no") return -1;
    char* end = nullptr;
    const double d = std::strtod(v.c_str(), &end);
    if (end == v.c_str() || !std::isfinite(d) || d < 0) return -1;
    return (int64_t)std::llround(d * 1000.0);
}
int64_t secToMs(double s) { return (s >= 0 && std::isfinite(s)) ? (int64_t)std::llround(s * 1000.0) : -1; }

// THE VOLUME (measured by the smoke with ao=pcm): mpv's software volume is CUBIC (gain = (volume / 100)^3), <video>.volume
// is LINEAR (gain = volume). The page's slider means the same loudness on both backends: pct -> mpv = 100 * cbrt(pct / 100).
double pctToMpv(int pct) {
    const double p = std::min(100.0, std::max(0.0, (double)pct)) / 100.0;
    return 100.0 * std::cbrt(p);
}
int mpvToPct(double v) {
    const double g = std::max(0.0, v) / 100.0;
    return (int)std::lround(std::min(1.3, g * g * g) * 100.0);
}

std::string endFileSentence(int error, const char* (*errStr)(int)) {
    const char* why = nullptr;
    switch (error) {
    case MPV_ERROR_LOADING_FAILED: why = "the file could not be opened"; break;
    case MPV_ERROR_AO_INIT_FAILED: why = "the sound output could not start"; break;
    case MPV_ERROR_VO_INIT_FAILED: why = "the video output could not start"; break;
    case MPV_ERROR_NOTHING_TO_PLAY: why = "it holds no sound or picture it can play"; break;
    case MPV_ERROR_UNKNOWN_FORMAT: why = "its format is not recognised"; break;
    case MPV_ERROR_UNSUPPORTED: why = "it is not supported"; break;
    default: break;
    }
    std::string s = "The video engine could not play this file: ";
    if (why) s += why;
    else s += (errStr && error < 0) ? errStr(error) : "unknown error";
    return s + ".";
}

}   // namespace

// =====================================================================================================================
// The implementation
// =====================================================================================================================
struct MpvPlayer::Impl {
    MpvOptions opt;
    bool loaded = false, shut = false;
    ULONGLONG loadMs = 0;                     // LoadLibraryExW of libmpv-2.dll (the UI thread, at the start: native.log says it)
    std::string vulkan;                       // N3: which Vulkan loader libmpv's import got (native.log)
    std::string why = "the native video engine has not started";
    HWND parent = nullptr, video = nullptr;
    mpv_handle* h = nullptr;
    std::atomic<bool> wakePending{false};
    std::atomic<HWND> wakeTo{nullptr};        // where mpv's threads and the probe post (the video window while mpv lives)
    std::function<void(const MpvEvent&)> sink;
    MpvState st;
    std::vector<MpvEvent> pending;            // made outside a drain (a refused command): handed over by the next drain

    // items: loadfile's reply -> the item's gen; mpv's playlist entry -> gen; the item mpv plays now
    uint64_t loadSeq = 0, curGen = 0, lastOpenGen = 0;
    int64_t curEntry = -1;
    std::map<uint64_t, uint64_t> replyGen;    // kReplyLoad | seq -> gen
    std::map<int64_t, uint64_t> entryGen;     // playlist_entry_id -> gen
    // the sound track: asked for (open / selectAudio), mpv's ids of the item's sound tracks in container order
    int wantOrd = -1;
    bool ordChecked = false, loadedSeen = false;
    std::vector<int64_t> audioIds;
    int64_t selectedId = -1;                  // mpv's id of the sound track heard (-1 = none)
    int64_t lastAbB = -1;
    bool voFailed = false;
    // MEDIUM3: mpv refuses a seek until the item is loaded (-12, measured): what is asked while the item LOADS -- a seek,
    // setAbLoop's jump to A, a frame step; the LAST one wins -- is held and done at the item's file-loaded
    uint64_t loadingGen = 0;                  // the item opened and not loaded yet (0 = none)
    struct Act { int kind = 0; int64_t ms = 0; bool exact = true; int step = 0; } held;   // kind 0 none, 1 seek, 2 frame
    // a held FRAME STEP waits one step more, for the item's first restart: mpv IGNORES a back step while its start seek
    // runs (hrseek_active -- measured 02/11: the step done at file-loaded of an item opened at 2 s left it at 2 s)
    uint64_t stepAtRestart = 0;               // the item whose first restart does the held frame step (0 = none)
    // L1 (page review): a seek asked since the last eof -- play() at the end must not restart over it (it is on its way)
    bool seekSinceEof = false;
    // every other command's refusal, logged by NAME (kReplyCmd | sequence -> the command)
    uint64_t cmdSeq = 0;
    std::map<uint64_t, std::string> cmdNames;

    // the 30 Hz clock
    ULONGLONG lastTimeEmit = 0;
    bool timePending = false;

    // placement: the page's CSS rectangle, the scale, full screen's interim fill
    double cx = 0, cy = 0, cw = 0, ch = 0, scale = 1.0;
    bool cssValid = false;
    bool fs = false, fillPending = false;
    double sx = 0, sy = 0, sw = 0, sh = 0;
    bool savedValid = false;
    RECT placed{};

    // the GPU probe (WARP?). LOW7: its thread writes ONLY into this shared block (it holds its own reference): if the end
    // gives up waiting for it (a GPU hang inside D3D11CreateDevice), it never writes into a freed Impl.
    struct Probe {
        std::atomic<int> result{-1};          // -1 running, 0 hardware, 1 software (WARP / the Basic Render Driver)
        std::string adapter;                  // written before `result` is set
        std::atomic<HWND> target{nullptr};    // where its answer is posted (cleared by the end)
    };
    std::shared_ptr<Probe> probeS = std::make_shared<Probe>();
    HANDLE probe = nullptr;
    bool probeHandled = false;

    // native.log
    HANDLE logFile = INVALID_HANDLE_VALUE;
    ULONGLONG logMinute = 0, logBytes = 0;
    int logLines = 0;

    // ---- log -----------------------------------------------------------------------------------------------------
    // Opened by load() (LOW9: a DLL that is missing or will not load is said in it too). Past kLogCapBytes it starts again
    // -- checked at the opening AND while it writes (a long session at the per-minute cap grew it by ~3.6 MB/h).
    void openLog(bool fresh = false) {
        if (opt.logPath.empty() || logFile != INVALID_HANDLE_VALUE) return;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        const bool big = fresh || (::GetFileAttributesExW(opt.logPath.c_str(), GetFileExInfoStandard, &fa)
                                   && (fa.nFileSizeHigh || fa.nFileSizeLow > kLogCapBytes));
        logFile = ::CreateFileW(opt.logPath.c_str(), big ? GENERIC_WRITE : FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, big ? CREATE_ALWAYS : OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        logBytes = big ? 0 : (static_cast<ULONGLONG>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow);
        if (logFile != INVALID_HANDLE_VALUE && big) writeLine("(native.log started again: it was over 1 MB)");
    }
    void closeLog() {
        if (logFile != INVALID_HANDLE_VALUE) ::CloseHandle(logFile);
        logFile = INVALID_HANDLE_VALUE;
    }
    void logf(const char* f, ...) {
        if (logFile == INVALID_HANDLE_VALUE) return;
        const ULONGLONG now = ::GetTickCount64();
        if (now - logMinute >= 60000) { logMinute = now; logLines = 0; }
        if (++logLines > kLogLinesPerMinute) {
            if (logLines == kLogLinesPerMinute + 1) writeLine("(more lines this minute were left out)");
            return;
        }
        char body[2048];
        va_list ap;
        va_start(ap, f);
        vsnprintf(body, sizeof body, f, ap);
        va_end(ap);
        writeLine(body);
    }
    void writeLine(const char* body) {
        std::string b(body);
        while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
        SYSTEMTIME t{};
        ::GetLocalTime(&t);
        char stamp[40];
        snprintf(stamp, sizeof stamp, "%04u-%02u-%02u %02u:%02u:%02u.%03u ", (unsigned)t.wYear, (unsigned)t.wMonth,
                 (unsigned)t.wDay, (unsigned)t.wHour, (unsigned)t.wMinute, (unsigned)t.wSecond, (unsigned)t.wMilliseconds);
        const std::string line = stamp + b + "\r\n";
        if (logBytes + line.size() > kLogCapBytes) {      // (LOW9: the cap, while it writes -- the file starts again)
            closeLog();
            openLog(/*fresh=*/true);
            if (logFile == INVALID_HANDLE_VALUE) return;
        }
        DWORD wr = 0;
        ::WriteFile(logFile, line.data(), (DWORD)line.size(), &wr, nullptr);
        logBytes += wr;
    }

    // ---- requests to mpv (async: the UI thread never waits on mpv's core) ----------------------------------------
    void cmd(std::initializer_list<const char*> args) {
        if (!h) return;
        std::vector<const char*> v(args);
        v.push_back(nullptr);
        const uint64_t tag = kReplyCmd | (++cmdSeq & 0xffffffffu);   // (its refusal comes back as a reply: logged by name)
        cmdNames[tag] = *args.begin();
        const int e = g_api.command_async(h, tag, v.data());
        if (e < 0) {
            cmdNames.erase(tag);
            logf("command %s refused: %s", *args.begin(), g_api.error_string(e));
        }
    }
    void setStr(const char* name, const std::string& value) {
        if (!h) return;
        const char* s = value.c_str();
        const int e = g_api.set_property_async(h, 0, name, MPV_FORMAT_STRING, &s);
        if (e < 0) logf("set %s=%s refused: %s", name, value.c_str(), g_api.error_string(e));
    }
    void setDouble(const char* name, double v) {
        if (!h) return;
        const int e = g_api.set_property_async(h, 0, name, MPV_FORMAT_DOUBLE, &v);
        if (e < 0) logf("set %s refused: %s", name, g_api.error_string(e));
    }
    void setFlag(const char* name, bool on) {
        if (!h) return;
        int f = on ? 1 : 0;
        const int e = g_api.set_property_async(h, 0, name, MPV_FORMAT_FLAG, &f);
        if (e < 0) logf("set %s refused: %s", name, g_api.error_string(e));
    }
    void setInt(const char* name, int64_t v) {
        if (!h) return;
        const int e = g_api.set_property_async(h, 0, name, MPV_FORMAT_INT64, &v);
        if (e < 0) logf("set %s refused: %s", name, g_api.error_string(e));
    }
    void seekTo(int64_t ms, bool exact) {
        const std::string t = fmt("%.3f", (double)std::max<int64_t>(0, ms) / 1000.0);
        cmd({"seek", t.c_str(), exact ? "absolute+exact" : "absolute+keyframes"});
    }
    // A seek ASKED (seek(), setAbLoop's jump to A): while the item loads it is held (the last one wins) and done at its
    // file-loaded; and play() at the end must not restart from 0 over it (L1).
    void askSeek(int64_t ms, bool exact) {
        seekSinceEof = true;
        if (loadingGen) {
            held = Act{1, ms, exact, 0};
            return;
        }
        dropHeldStep();                                   // (asked later: it wins over a step still waiting for the restart)
        seekTo(ms, exact);
    }
    void askFrame(int step) {
        if (loadingGen) {
            held = Act{2, 0, true, step};
            return;
        }
        dropHeldStep();
        if (step > 0) cmd({"frame-step"});
        else if (step < 0) cmd({"frame-back-step"});
    }
    void dropHeldStep() {
        if (!stepAtRestart) return;
        stepAtRestart = 0;
        held = Act{};
    }
    // the item is loaded: what was asked meanwhile is done now
    void replayHeld() {
        const Act a = held;
        held = Act{};
        if (a.kind == 1) {
            logf("the seek asked while item %llu loaded: %.3f s, done now", (unsigned long long)curGen, (double)a.ms / 1000.0);
            seekTo(a.ms, a.exact);
        } else if (a.kind == 2) {
            logf("the frame step asked while item %llu loaded: done now", (unsigned long long)curGen);
            if (a.step > 0) cmd({"frame-step"});
            else if (a.step < 0) cmd({"frame-back-step"});
        }
    }

    // ---- events ---------------------------------------------------------------------------------------------------
    MpvEvent ev(MpvEvent::Kind k) const {
        MpvEvent e;
        e.kind = k;
        e.gen = curGen;
        return e;
    }
    void post() {
        if (video && !wakePending.exchange(true))
            if (!::PostMessageW(video, kWmWakeup, 0, 0)) wakePending.store(false);
    }
    void deliver(std::vector<MpvEvent>& out) {
        for (const MpvEvent& e : out) {
            if (!sink) break;
            const auto s = sink;                         // (the sink may replace itself)
            s(e);
            if (shut) break;                             // (the sink shut the engine down: nothing more to say)
        }
        out.clear();
    }
    void flushTime(std::vector<MpvEvent>& out) {
        if (video) ::KillTimer(video, kTimerTime);
        if (!timePending) return;
        timePending = false;
        lastTimeEmit = nowMs();
        MpvEvent e = ev(MpvEvent::Kind::Time);
        e.ms = st.timeMs;
        out.push_back(e);
    }
    void volumeEvent(std::vector<MpvEvent>& out) {
        MpvEvent e = ev(MpvEvent::Kind::Volume);
        e.value = st.volPct;
        e.flag = st.muted;
        out.push_back(e);
    }
    void resetItem() {
        st.timeMs = -1;
        st.durMs = -1;
        st.eof = false;
        st.seeking = false;
        st.seekable = false;
        st.ord = -1;
        st.audioTracks = 0;
        st.audioList.clear();                     // (the contract: reset at Start, filled at Loaded)
        st.hasVideo = false;
        // (NOT the A-B loop: mpv's ab-loop-a / -b are global and outlive a file -- the state mirrors mpv's own, which open()
        // clears; a reset HERE would hide a loop mpv still holds: with A off and B set, mpv loops from the start to B)
        // (NOT the picture's size either: mpv's dwidth / dheight are observed, and mpv does not say them again for a next item
        // of the SAME size -- measured 02/11: reset here, every next 640x360 sample of a row of them stayed 0 x 0; mpv does
        // say 0 when a file has no picture: the state mirrors mpv's own)
        st.hwdec.clear();
        audioIds.clear();
        selectedId = -1;
        ordChecked = false;
        loadedSeen = false;
        voFailed = false;
        timePending = false;
    }

    // The track asked for at open() went as aid=ord+1 (no switch at the start). Once the item is loaded it is checked
    // against the track mpv REALLY plays: a mismatch (mpv's numbering not the container's) is put right. While mpv has told no
    // choice yet -- its track list can come before its selection (measured 26/10: "selected" still none at file-loaded) --
    // nothing is done: setting aid again would re-start the sound chain for nothing. At the first playback restart the
    // choice is final; if there is still none (an mpv that, given an aid the item lacks, plays NO sound), the asked track if
    // it exists, else mpv's own choice. (This libmpv falls back to its default track by itself: measured 26/10.)
    void checkOrd(bool final) {
        if (!curGen || ordChecked || !loadedSeen || audioIds.empty() || wantOrd < 0) return;
        const size_t n = audioIds.size();
        if (selectedId >= 0) {
            ordChecked = true;
            if ((size_t)wantOrd < n && audioIds[(size_t)wantOrd] != selectedId) {
                logf("sound track %d is mpv's id %lld (it played %lld): selected", wantOrd, (long long)audioIds[(size_t)wantOrd],
                     (long long)selectedId);
                setInt("aid", audioIds[(size_t)wantOrd]);
            }
            return;
        }
        if (!final) return;
        ordChecked = true;
        logf("no sound track playing at the first restart (track %d asked for, %zu in the item): %s", wantOrd, n,
             (size_t)wantOrd < n ? "the asked one" : "mpv's own choice");
        if ((size_t)wantOrd < n) setInt("aid", audioIds[(size_t)wantOrd]);
        else setStr("aid", "auto");
    }

    // mpv's track-list (an array of maps): the sound tracks in ORD order with what the menu shows (the contract's
    // audioList), the heard one, and whether a PICTURE track exists (not cover art: an mp3's attached picture is "albumart").
    // `atLoad` = the list read at file-loaded: Tracks is told then (BEFORE Loaded) -- and afterwards whenever it changes
    // (MPEG-TS can add a stream mid-file); before the load the property's partial lists only feed the state.
    void applyTracks(const mpv_node* arr, std::vector<MpvEvent>& out, bool atLoad) {
        std::vector<MpvState::Audio> list;
        int64_t selected = -1;
        bool picture = false;
        if (arr && arr->format == MPV_FORMAT_NODE_ARRAY && arr->u.list) {
            for (int i = 0; i < arr->u.list->num; ++i) {
                const mpv_node& t = arr->u.list->values[i];
                if (nodeFlag(mapGet(t, "external"))) continue;
                const std::string type = nodeStr(mapGet(t, "type"));
                if (type == "video" && !nodeFlag(mapGet(t, "albumart"))) picture = true;
                if (type != "audio") continue;
                MpvState::Audio a;
                a.id = nodeInt(mapGet(t, "id"), -1);
                if (a.id < 0) continue;
                const mpv_node* n = mapGet(t, "lang");
                a.lang = fileText(n && n->format == MPV_FORMAT_STRING ? n->u.string : nullptr, 16);
                n = mapGet(t, "title");
                a.title = fileText(n && n->format == MPV_FORMAT_STRING ? n->u.string : nullptr, 160);
                n = mapGet(t, "codec");
                a.codec = fileText(n && n->format == MPV_FORMAT_STRING ? n->u.string : nullptr, 32);
                a.channels = (int)std::min<int64_t>(64, std::max<int64_t>(0, nodeInt(mapGet(t, "demux-channel-count"), 0)));
                a.isDefault = nodeFlag(mapGet(t, "default"));
                if (nodeFlag(mapGet(t, "selected"))) selected = a.id;
                list.push_back(a);
            }
        }
        // mpv numbers each kind of track in the demuxer's order, which is the container's: the ord-th sound track is the
        // ord-th id in increasing order (the SAME ord as MediaTracks / TrPipe)
        std::sort(list.begin(), list.end(), [](const MpvState::Audio& a, const MpvState::Audio& b) { return a.id < b.id; });
        std::vector<int64_t> ids;
        for (const MpvState::Audio& a : list) ids.push_back(a.id);
        const auto same = [](const MpvState::Audio& a, const MpvState::Audio& b) {
            return a.id == b.id && a.lang == b.lang && a.title == b.title && a.codec == b.codec && a.channels == b.channels
                   && a.isDefault == b.isDefault;
        };
        const bool changed = picture != st.hasVideo || list.size() != st.audioList.size()
                             || !std::equal(list.begin(), list.end(), st.audioList.begin(), same);
        audioIds = ids;
        selectedId = selected;
        st.audioTracks = (int)ids.size();
        st.audioList = list;
        st.hasVideo = picture;
        int ord = -1;
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == selected) ord = (int)i;
        checkOrd(false);
        if (atLoad || (loadedSeen && changed)) out.push_back(ev(MpvEvent::Kind::Tracks));
        if (ord != st.ord) {
            st.ord = ord;
            MpvEvent e = ev(MpvEvent::Kind::Track);
            e.ord = ord;
            out.push_back(e);
        }
    }
    void onTracks(const mpv_event_property& p, std::vector<MpvEvent>& out) {
        applyTracks(p.format == MPV_FORMAT_NODE && p.data ? static_cast<const mpv_node*>(p.data) : nullptr, out, false);
    }
    // The item is LOADED, its track list in hand: Tracks, then Loaded (the contract's order: both known at Loaded) -- then
    // what was asked while it loaded (the last seek / frame step), mpv accepting it from now on.
    void fileLoaded(const mpv_node* list, std::vector<MpvEvent>& out) {
        if (list) applyTracks(list, out, true);
        else out.push_back(ev(MpvEvent::Kind::Tracks));   // (no answer: the list the property gave)
        checkOrd(false);
        out.push_back(ev(MpvEvent::Kind::Loaded));
        if (loadingGen && curGen == loadingGen) {
            loadingGen = 0;
            if (held.kind == 2) stepAtRestart = curGen;   // (a frame step: at the first restart, see stepAtRestart)
            else replayHeld();
        }
    }

    void onProperty(uint64_t id, const mpv_event_property& p, std::vector<MpvEvent>& out) {
        switch (id) {
        case P_TIME: {
            const int64_t ms = secToMs(propDouble(p, -1.0));
            if (ms == st.timeMs) break;
            st.timeMs = ms;
            timePending = true;
            const ULONGLONG now = nowMs();
            if (now - lastTimeEmit >= kTimeEveryMs) flushTime(out);
            else if (video) ::SetTimer(video, kTimerTime, (UINT)(kTimeEveryMs - (now - lastTimeEmit)), nullptr);
            break;
        }
        case P_DUR: {
            const int64_t ms = secToMs(propDouble(p, -1.0));
            if (ms == st.durMs) break;
            st.durMs = ms;
            MpvEvent e = ev(MpvEvent::Kind::Duration);
            e.ms = ms;
            out.push_back(e);
            break;
        }
        case P_PAUSE: {
            const bool v = propFlag(p, st.paused);
            if (v == st.paused) break;
            st.paused = v;
            flushTime(out);
            MpvEvent e = ev(MpvEvent::Kind::Pause);
            e.flag = v;
            out.push_back(e);
            break;
        }
        case P_EOF: {
            const bool v = propFlag(p, false);
            if (v == st.eof) break;
            st.eof = v;
            if (v) seekSinceEof = false;                 // (L1: a NEW end -- play() restarts from 0 unless a seek follows)
            flushTime(out);
            MpvEvent e = ev(MpvEvent::Kind::Eof);
            e.flag = v;
            out.push_back(e);
            break;
        }
        case P_SPEED: {
            const double v = propDouble(p, st.speed);
            if (v == st.speed) break;
            st.speed = v;
            flushTime(out);
            MpvEvent e = ev(MpvEvent::Kind::Speed);
            e.value = v;
            out.push_back(e);
            break;
        }
        case P_VOL: {
            const int v = mpvToPct(propDouble(p, 100.0));
            if (v == st.volPct) break;
            st.volPct = v;
            volumeEvent(out);
            break;
        }
        case P_MUTE: {
            const bool v = propFlag(p, st.muted);
            if (v == st.muted) break;
            st.muted = v;
            volumeEvent(out);
            break;
        }
        case P_ABA:
        case P_ABB: {
            const int64_t v = abMs(propStr(p));
            int64_t& slot = id == P_ABA ? st.abA : st.abB;
            if (v == slot) break;
            slot = v;
            MpvEvent e = ev(MpvEvent::Kind::AbLoop);
            e.abA = st.abA;
            e.abB = st.abB;
            out.push_back(e);
            break;
        }
        case P_DW:
        case P_DH: {
            const int v = (int)std::max<int64_t>(0, propInt(p, 0));
            int& slot = id == P_DW ? st.w : st.h;
            if (v == slot) break;
            slot = v;
            MpvEvent e = ev(MpvEvent::Kind::VideoSize);
            e.w = st.w;
            e.h = st.h;
            out.push_back(e);
            break;
        }
        case P_HWDEC: {
            std::string v = propStr(p);
            if (p.format == MPV_FORMAT_NONE) v.clear();          // (no video decoder now)
            if (v == st.hwdec) break;
            st.hwdec = v;
            if (!v.empty()) logf("decoding: %s%s", v == "no" ? "software (FFmpeg)" : v.c_str(), st.warp ? ", rendering in software (WARP)" : "");
            MpvEvent e = ev(MpvEvent::Kind::Hwdec);
            e.text = v.empty() ? std::string("no") : v;
            e.flag = st.warp;
            out.push_back(e);
            break;
        }
        case P_TRACKS: onTracks(p, out); break;
        case P_SEEKING: {
            const bool v = propFlag(p, false);
            if (v == st.seeking) break;
            st.seeking = v;
            flushTime(out);
            MpvEvent e = ev(MpvEvent::Kind::Seeking);
            e.flag = v;
            out.push_back(e);
            break;
        }
        case P_SEEKABLE: {
            const bool v = propFlag(p, false);
            if (v == st.seekable) break;
            st.seekable = v;
            MpvEvent e = ev(MpvEvent::Kind::Seekable);
            e.flag = v;
            out.push_back(e);
            break;
        }
        case P_IDLE: {
            const bool v = propFlag(p, false);
            if (v == st.idle) break;
            st.idle = v;
            MpvEvent e = ev(MpvEvent::Kind::Idle);
            e.flag = v;
            out.push_back(e);
            break;
        }
        case P_VO:
        case P_VCODEC:
        case P_ACODEC: {
            const std::string v = propStr(p);
            if (!v.empty())
                logf("%s: %s", id == P_VO ? "video output" : id == P_VCODEC ? "video codec" : "audio codec", v.c_str());
            break;
        }
        default: break;
        }
    }

    void handle(const mpv_event& e, std::vector<MpvEvent>& out) {
        switch (e.event_id) {
        case MPV_EVENT_LOG_MESSAGE: {
            const mpv_event_log_message* m = static_cast<const mpv_event_log_message*>(e.data);
            if (!m) break;
            logf("mpv[%s] %s: %s", m->prefix ? m->prefix : "?", m->level ? m->level : "?", m->text ? m->text : "");
            // the video output could not start (no usable Direct3D 11, even WARP): mpv goes on with the sound only --
            // the picture is the page's to show then (the html5 fallback for this item)
            if (m->log_level <= MPV_LOG_LEVEL_ERROR && m->text && std::strstr(m->text, "video_out") && curGen && !voFailed) {
                voFailed = true;
                MpvEvent x = ev(MpvEvent::Kind::Error);
                x.text = endFileSentence(MPV_ERROR_VO_INIT_FAILED, g_api.error_string);
                out.push_back(x);
            }
            break;
        }
        case MPV_EVENT_PROPERTY_CHANGE:
            if (e.data) onProperty(e.reply_userdata, *static_cast<const mpv_event_property*>(e.data), out);
            break;
        case MPV_EVENT_COMMAND_REPLY: {
            if ((e.reply_userdata & kReplyLoad) == 0) {
                const auto n = cmdNames.find(e.reply_userdata);
                const std::string name = n != cmdNames.end() ? n->second : std::string("a command");
                if (n != cmdNames.end()) cmdNames.erase(n);
                if (e.error < 0) logf("%s refused by mpv: %s (item %llu)", name.c_str(), g_api.error_string(e.error),
                                      (unsigned long long)curGen);
                break;
            }
            const auto it = replyGen.find(e.reply_userdata);
            const uint64_t gen = it != replyGen.end() ? it->second : 0;
            if (it != replyGen.end()) replyGen.erase(it);
            if (e.error < 0) {
                logf("loadfile refused (item %llu): %s", (unsigned long long)gen, g_api.error_string(e.error));
                MpvEvent x;
                x.kind = MpvEvent::Kind::Error;
                x.gen = gen;
                x.text = endFileSentence(e.error, g_api.error_string);
                out.push_back(x);
                break;
            }
            // loadfile answers with the playlist entry it made: the item's events are recognised by it
            const mpv_event_command* c = static_cast<const mpv_event_command*>(e.data);
            const int64_t entry = c ? nodeInt(mapGet(c->result, "playlist_entry_id"), -1) : -1;
            if (entry >= 0) entryGen[entry] = gen;
            break;
        }
        case MPV_EVENT_START_FILE: {
            const mpv_event_start_file* s = static_cast<const mpv_event_start_file*>(e.data);
            const int64_t entry = s ? s->playlist_entry_id : -1;
            const auto it = entryGen.find(entry);
            // (an mpv without loadfile's answer: the item asked for last)
            curGen = it != entryGen.end() ? it->second : lastOpenGen;
            curEntry = entry;
            st.gen = curGen;
            resetItem();
            logf("start-file: item %llu (entry %lld)", (unsigned long long)curGen, (long long)entry);
            out.push_back(ev(MpvEvent::Kind::Start));
            break;
        }
        case MPV_EVENT_FILE_LOADED: {
            logf("file-loaded: item %llu", (unsigned long long)curGen);
            loadedSeen = true;
            // The track list as it stands NOW (the property's changes may still be on their way): asked ASYNCHRONOUSLY --
            // mpv makes its video window right after file-loaded, a cross-thread child of ours, so this thread must never
            // wait on mpv's core here -- and Tracks then Loaded are told with the answer (fileLoaded()).
            const uint64_t tag = kReplyTracks | (curGen & 0xffffffffffULL);
            if (!g_api.get_property_async || g_api.get_property_async(h, tag, "track-list", MPV_FORMAT_NODE) < 0) fileLoaded(nullptr, out);
            break;
        }
        case MPV_EVENT_GET_PROPERTY_REPLY: {
            if ((e.reply_userdata & kReplyTracks) == 0) break;
            if ((e.reply_userdata & 0xffffffffffULL) != (curGen & 0xffffffffffULL) || !loadedSeen) break;   // (another item's)
            const mpv_event_property* p = static_cast<const mpv_event_property*>(e.data);
            fileLoaded(e.error >= 0 && p && p->format == MPV_FORMAT_NODE && p->data ? static_cast<const mpv_node*>(p->data) : nullptr,
                   out);
            break;
        }
        case MPV_EVENT_PLAYBACK_RESTART:
            flushTime(out);
            checkOrd(true);                               // (the sound chain is up: mpv's choice of track is final)
            out.push_back(ev(MpvEvent::Kind::Restart));
            if (stepAtRestart && stepAtRestart == curGen) {   // the item's start position reached: its held frame step now
                stepAtRestart = 0;
                replayHeld();
            }
            break;
        case MPV_EVENT_END_FILE: {
            const mpv_event_end_file* f = static_cast<const mpv_event_end_file*>(e.data);
            if (!f) break;
            const auto it = entryGen.find(f->playlist_entry_id);
            const uint64_t gen = it != entryGen.end() ? it->second : (f->playlist_entry_id == curEntry ? curGen : 0);
            if (it != entryGen.end()) entryGen.erase(it);
            logf("end-file: item %llu reason %d%s%s", (unsigned long long)gen, (int)f->reason, f->error < 0 ? " error: " : "",
                 f->error < 0 ? g_api.error_string(f->error) : "");
            if (gen && gen == loadingGen) {                  // the item being loaded ended before it loaded: what it held goes
                loadingGen = 0;
                held = Act{};
            }
            if (gen && gen == stepAtRestart) {               // (or before its first restart)
                stepAtRestart = 0;
                held = Act{};
            }
            if (f->reason == MPV_END_FILE_REASON_ERROR) {
                MpvEvent x;
                x.kind = MpvEvent::Kind::Error;
                x.gen = gen;
                x.text = endFileSentence(f->error, g_api.error_string);
                out.push_back(x);
            }
            if (f->playlist_entry_id == curEntry) {          // the item mpv played is over: the events now belong to none
                flushTime(out);
                curGen = 0;
                curEntry = -1;
                st.gen = 0;
            }
            break;
        }
        case MPV_EVENT_QUEUE_OVERFLOW: logf("mpv's event queue overflowed (the UI thread was busy)"); break;
        case MPV_EVENT_SHUTDOWN:
            // mpv ended by itself (a fatal error): the engine is gone -- said, and the handle let go off this drain
            logf("mpv shut down by itself");
            if (video) ::PostMessageW(video, kWmMpvGone, 0, 0);
            break;
        default: break;
        }
    }

    void drain() {
        wakePending.store(false);                         // before draining: an event arriving meanwhile posts again
        std::vector<MpvEvent> out;
        out.swap(pending);
        deliver(out);
        for (int n = 0; h && !shut; ++n) {
            if (n >= 2000) { post(); break; }             // a flood: the rest after the other messages (never starve the UI)
            mpv_event* e = g_api.wait_event(h, 0);
            if (!e || e->event_id == MPV_EVENT_NONE) break;
            handle(*e, out);                              // (copies what it needs: the sink may call back into mpv)
            if (!out.empty()) deliver(out);
        }
    }

    // ---- the GPU probe ---------------------------------------------------------------------------------------------
    // Is there a GPU Direct3D 11 can use? mpv's d3d11-warp=auto falls back to WARP when the hardware device fails; on a PC
    // with no display driver at all, the "hardware" device is the Basic Render Driver -- software too. Either way mpv then
    // renders on the CPU, where its default scalers cost 900-1200 % of a core (N0, measured): profile=fast (49 %).
    // LOW7: the thread gets its OWN reference to the result block (a heap shared_ptr it deletes): whatever happens to this
    // Impl -- the end gives up after 2 s on a D3D11CreateDevice stuck in a hung driver -- it writes only there.
    static DWORD WINAPI probeProc(void* p) {
        std::unique_ptr<std::shared_ptr<Probe>> own(static_cast<std::shared_ptr<Probe>*>(p));
        Probe* self = own->get();
        int result = 1;
        std::string adapter = "no Direct3D 11 hardware device";
        HMODULE d3d = ::LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto create = d3d ? reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(reinterpret_cast<void*>(::GetProcAddress(d3d, "D3D11CreateDevice")))
                          : nullptr;
        ID3D11Device* dev = nullptr;
        if (create && SUCCEEDED(create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                       nullptr, nullptr)) && dev) {
            IDXGIDevice* dx = nullptr;
            if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dx))) && dx) {
                IDXGIAdapter* ad = nullptr;
                if (SUCCEEDED(dx->GetAdapter(&ad)) && ad) {
                    DXGI_ADAPTER_DESC d{};
                    if (SUCCEEDED(ad->GetDesc(&d))) {
                        // the Microsoft Basic Render Driver (VEN 1414 DEV 008C): what Windows renders with when no
                        // display driver is installed -- a software rasteriser behind a "hardware" device
                        const bool basic = d.VendorId == 0x1414 && d.DeviceId == 0x8c;
                        result = basic ? 1 : 0;
                        adapter = u8(d.Description) + fmt(" (VEN_%04X DEV_%04X)", d.VendorId, d.DeviceId);
                    }
                    ad->Release();
                }
                dx->Release();
            }
            dev->Release();
        }
        self->adapter = adapter;
        self->result.store(result);
        if (HWND w = self->target.load()) ::PostMessageW(w, kWmProbe, 0, 0);
        return 0;
    }
    // LOW8: the answer is applied WHEN it comes -- open() never waits for it (it waited up to 3 s on the UI thread). A first
    // file opened before it starts with mpv's default scalers; apply-profile switches them as soon as the answer says WARP.
    void onProbe() {
        if (probeHandled || probeS->result.load() < 0) return;
        probeHandled = true;
        const bool soft = probeS->result.load() == 1;
        logf("GPU: %s -> %s", probeS->adapter.c_str(), soft ? "rendering in software (WARP): profile=fast" : "hardware rendering");
        if (soft && h && !st.warp) {
            st.warp = true;
            cmd({"apply-profile", "fast"});
            MpvEvent e = ev(MpvEvent::Kind::Hwdec);       // (warp is told with the decoder)
            e.text = st.hwdec.empty() ? std::string("no") : st.hwdec;
            e.flag = true;
            pending.push_back(e);
            post();
        }
    }

    // ---- placement --------------------------------------------------------------------------------------------------
    void placeRect(const RECT& r) {
        if (!video) return;
        const bool empty = r.right <= r.left || r.bottom <= r.top;
        if (empty) {
            if (::IsWindowVisible(video)) ::ShowWindow(video, SW_HIDE);
            placed = RECT{};
            return;
        }
        if (::EqualRect(&r, &placed) && ::IsWindowVisible(video)) return;
        placed = r;
        ::SetWindowPos(video, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top,
                       SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }
    void applyCss() {
        if (!cssValid) return;
        if (cw <= 0 || ch <= 0) { placeRect(RECT{}); return; }
        const RECT r{(LONG)std::lround(cx * scale), (LONG)std::lround(cy * scale), (LONG)std::lround((cx + cw) * scale),
                     (LONG)std::lround((cy + ch) * scale)};
        placeRect(r);
    }
    void fill() {
        if (!video || !parent) return;
        if (!(cssValid && cw > 0 && ch > 0)) return;     // no picture shown (an audio file, no item): nothing to fill
        RECT rc{};
        ::GetClientRect(parent, &rc);
        placeRect(rc);
    }

    // ---- the end -----------------------------------------------------------------------------------------------------
    static DWORD WINAPI terminateProc(void* p) {
        g_api.terminate_destroy(static_cast<mpv_handle*>(p));
        return 0;
    }
    void terminate() {
        if (!h) return;
        mpv_handle* hh = h;
        h = nullptr;
        // (mpv calls the wakeup with its lock held: once this returns, no callback runs or follows)
        g_api.set_wakeup_callback(hh, nullptr, nullptr);
        const ULONGLONG t0 = ::GetTickCount64(), p0 = nowMs();
        bool done = true;
        HANDLE th = ::CreateThread(nullptr, 0, terminateProc, hh, 0, nullptr);
        if (!th) {
            g_api.terminate_destroy(hh);                  // (no thread: the blocking way, as libmpv documents it)
        } else {
            // mpv's window is a child of ours on mpv's thread: destroying it SENDS messages to our thread (the parent's
            // notifications), so this thread answers sent messages while it waits -- and only those (no timer, no post
            // of ours runs in the middle of the app's end)
            for (;;) {
                const ULONGLONG spent = ::GetTickCount64() - t0;
                if (spent >= 15000) { done = false; break; }
                const DWORD r = ::MsgWaitForMultipleObjectsEx(1, &th, (DWORD)(15000 - spent), QS_SENDMESSAGE, 0);
                if (r == WAIT_OBJECT_0) break;
                if (r == WAIT_OBJECT_0 + 1) {
                    MSG m;
                    ::PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
                    continue;
                }
                done = false;
                break;
            }
            ::CloseHandle(th);
        }
        logf("mpv terminated%s in %llu ms", done ? "" : " (NOT finished: given up)", (unsigned long long)(nowMs() - p0));
        if (!done) video = nullptr;                       // (its window may still be alive in ours: never destroyed under it)
    }
    void shutdownAll() {
        if (shut) return;
        shut = true;
        if (video) ::KillTimer(video, kTimerTime);
        const bool was = h != nullptr;
        terminate();
        probeS->target.store(nullptr);                    // (a late answer posts nowhere: the block it writes is its own)
        if (probe) {
            ::WaitForSingleObject(probe, 2000);           // (LOW7: given up after 2 s, the thread still writes only its block)
            ::CloseHandle(probe);
            probe = nullptr;
        }
        wakeTo.store(nullptr);
        if (video) ::DestroyWindow(video);
        video = nullptr;
        if (was) why = "the video engine was shut down";
        closeLog();
    }

    static LRESULT CALLBACK wndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
        Impl* self = reinterpret_cast<Impl*>(::GetWindowLongPtrW(w, GWLP_USERDATA));
        switch (m) {
        case WM_NCCREATE: {
            const CREATESTRUCTW* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            ::SetWindowLongPtrW(w, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }
        case kWmWakeup:
            if (self) self->drain();
            return 0;
        case kWmProbe:
            if (self) self->onProbe();
            return 0;
        case kWmMpvGone:
            if (self && !self->shut) {
                self->why = "the video engine stopped by itself";
                std::vector<MpvEvent> out;
                MpvEvent e = self->ev(MpvEvent::Kind::Available);
                e.flag = false;
                e.text = self->why;
                out.push_back(e);
                self->terminate();                        // (off the drain: the handle is let go here)
                self->deliver(out);
            }
            return 0;
        case WM_TIMER:
            if (self && wp == kTimerTime) {
                std::vector<MpvEvent> out;
                self->flushTime(out);
                self->deliver(out);
            }
            return 0;
        case WM_NCHITTEST:
            return HTTRANSPARENT;                         // (belt and braces: a disabled child is skipped anyway)
        case WM_ERASEBKGND: {
            RECT rc;
            ::GetClientRect(w, &rc);
            ::FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH)));
            return 1;
        }
        case WM_NCDESTROY:
            ::SetWindowLongPtrW(w, GWLP_USERDATA, 0);
            if (self && self->video == w) {
                self->video = nullptr;
                self->wakeTo.store(nullptr);
                self->probeS->target.store(nullptr);
            }
            break;
        default: break;
        }
        return ::DefWindowProcW(w, m, wp, lp);
    }
    static void wakeup(void* p) {                         // mpv's threads, a lock held: only a post, coalesced
        Impl* self = static_cast<Impl*>(p);
        HWND w = self->wakeTo.load();
        if (w && !self->wakePending.exchange(true))
            if (!::PostMessageW(w, kWmWakeup, 0, 0)) self->wakePending.store(false);
    }
};

// =====================================================================================================================
// MpvPlayer
// =====================================================================================================================
MpvPlayer::MpvPlayer() : impl_(new Impl) {}

MpvPlayer::~MpvPlayer() {
    if (impl_) impl_->shutdownAll();
}

std::wstring MpvPlayer::defaultDllDir() {
    const std::wstring dir = exeDir();
    const std::wstring sub = dir + L"\\mpv";
    if (::GetFileAttributesW((sub + L"\\" + kDllName).c_str()) != INVALID_FILE_ATTRIBUTES) return sub;
    return dir;
}

bool MpvPlayer::load(const std::wstring& dllDir, std::string& why) {
    Impl& I = *impl_;
    I.openLog();                                       // (LOW9: FIRST -- a DLL missing or refused is said in native.log too)
    if (g_api.dll) { I.loaded = true; why.clear(); return true; }      // (once per process: never unloaded)
    std::wstring path = dllDir;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path += L'\\';
    path += kDllName;
    if (::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        why = "libmpv-2.dll is not installed (looked for " + u8(path) + ")";
        I.why = why;
        I.logf("---- SuVtitle native engine: NOT available -- %s", why.c_str());
        return false;
    }
    // N3: the Vulkan loader BEFORE libmpv (its import of vulkan-1.dll): the PC's own, else the bundled vkfallback\ one
    I.vulkan = preloadVulkanFallback(dllDir);
    // DLL_LOAD_DIR first: a DLL libmpv imports put next to it is found before System32's (never vulkan-1.dll: see above)
    const ULONGLONG t0 = nowMs();
    HMODULE m = ::LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    I.loadMs = nowMs() - t0;
    if (!m) {
        const DWORD e = ::GetLastError();
        why = fmt("libmpv-2.dll could not be loaded (Windows error %lu%s)", e,
                  e == ERROR_MOD_NOT_FOUND ? (": a DLL it needs is missing, vulkan-1.dll for this build (Vulkan loader: "
                                              + I.vulkan + ")").c_str() : "");
        I.why = why;
        I.logf("---- SuVtitle native engine: NOT available -- %s", why.c_str());
        return false;
    }
    Api a;
    a.dll = m;
    std::string missing;
    bool ok = true;
    ok = sym(m, a.client_api_version, "mpv_client_api_version", missing) && ok;
    ok = sym(m, a.error_string, "mpv_error_string", missing) && ok;
    ok = sym(m, a.free_mem, "mpv_free", missing) && ok;
    ok = sym(m, a.create, "mpv_create", missing) && ok;
    ok = sym(m, a.initialize, "mpv_initialize", missing) && ok;
    ok = sym(m, a.terminate_destroy, "mpv_terminate_destroy", missing) && ok;
    ok = sym(m, a.set_option, "mpv_set_option", missing) && ok;
    ok = sym(m, a.set_option_string, "mpv_set_option_string", missing) && ok;
    ok = sym(m, a.command_async, "mpv_command_async", missing) && ok;
    ok = sym(m, a.command_node_async, "mpv_command_node_async", missing) && ok;
    ok = sym(m, a.set_property_async, "mpv_set_property_async", missing) && ok;
    ok = sym(m, a.get_property_string, "mpv_get_property_string", missing) && ok;
    ok = sym(m, a.get_property_async, "mpv_get_property_async", missing) && ok;
    ok = sym(m, a.observe_property, "mpv_observe_property", missing) && ok;
    ok = sym(m, a.request_log_messages, "mpv_request_log_messages", missing) && ok;
    ok = sym(m, a.wait_event, "mpv_wait_event", missing) && ok;
    ok = sym(m, a.set_wakeup_callback, "mpv_set_wakeup_callback", missing) && ok;
    if (!ok) {
        why = "libmpv-2.dll lacks " + missing;
        I.why = why;
        I.logf("---- SuVtitle native engine: NOT available -- %s", why.c_str());
        ::FreeLibrary(m);                                  // (nothing of it ran: safe to let go)
        return false;
    }
    const unsigned long ver = a.client_api_version();
    if ((ver >> 16) != kApiMajor) {
        why = fmt("libmpv-2.dll speaks client API %lu.%lu (2.x needed)", ver >> 16, ver & 0xffff);
        I.why = why;
        I.logf("---- SuVtitle native engine: NOT available -- %s", why.c_str());
        ::FreeLibrary(m);
        return false;
    }
    g_api = a;
    I.loaded = true;
    I.logf("---- SuVtitle native engine: libmpv client API %lu.%lu, libmpv-2.dll loaded in %llu ms from %s; Vulkan loader: %s",
           ver >> 16, ver & 0xffff, (unsigned long long)I.loadMs, u8(dllDir).c_str(), I.vulkan.c_str());
    why.clear();
    return true;
}

// MEDIUM4 (docs\REVIEW_RV-NH.md): the process's DLL search, LOCKED before anything is loaded by name. Windows' default puts
// the CURRENT directory on the plain LoadLibrary path, and Explorer starts SuVtitle (--play) in the film's folder: libmpv
// loads DLLs by name at run time that most PCs lack (nvcuda, d3dcompiler_43/46, libEGL, VSScript, libbluray... -- the
// reviewer's probe), so a DLL of one of those names in a downloaded folder would have run. From here on a name is searched
// in the exe's folder, System32 and <exe dir>\mpv\ (the bundle's libmpv folder) ONLY. NEVER <exe dir>\mpv\vkfallback\: the
// folders given to AddDllDirectory come BEFORE System32 -- a Vulkan loader there would beat the PC's own (N3's rule).
bool MpvPlayer::lockDllSearch() {
    if (!::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return false;
    const std::wstring sub = exeDir() + L"\\mpv";
    const DWORD a = ::GetFileAttributesW(sub.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ::AddDllDirectory(sub.c_str());
    return true;
}

bool MpvPlayer::loaded() const { return impl_->loaded; }

void MpvPlayer::setOptions(const MpvOptions& o) {
    if (!impl_->h) impl_->opt = o;
}

bool MpvPlayer::attach(HWND parent) {
    Impl& I = *impl_;
    if (I.h) return true;
    if (I.shut || !g_api.dll || !parent) {
        if (!g_api.dll && I.why.empty()) I.why = "libmpv-2.dll is not loaded";
        return false;
    }
    I.parent = parent;
    HINSTANCE inst = ::GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW vc{};
        vc.cbSize = sizeof(vc);
        vc.lpfnWndProc = &Impl::wndProc;
        vc.hInstance = inst;
        vc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
        vc.lpszClassName = kVideoClass;
        registered = ::RegisterClassExW(&vc) != 0;
    }
    // DISABLED: hit-testing skips it and the window mpv makes inside it -- the mouse always comes to the parent. Hidden
    // until the page places the picture.
    I.video = ::CreateWindowExW(WS_EX_NOPARENTNOTIFY, kVideoClass, L"",
                                WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, nullptr, inst,
                                &I);
    if (!I.video) {
        I.why = fmt("the video window could not be created (Windows error %lu)", ::GetLastError());
        return false;
    }
    I.wakeTo.store(I.video);
    I.probeS->target.store(I.video);
    I.openLog();                                          // (load() opened it already; a second MpvPlayer opens its own)
    I.logf("mpv attached to the player's window%s", I.opt.silent ? " (silent: a test instance)" : "");
    I.h = g_api.create();
    if (!I.h) {
        I.why = "mpv could not be created";
        I.logf("%s", I.why.c_str());
        ::DestroyWindow(I.video);
        I.video = nullptr;
        return false;
    }
    auto opt = [&I](const char* k, const char* v) {
        const int e = g_api.set_option_string(I.h, k, v);
        if (e < 0) I.logf("option %s=%s: %s", k, v, g_api.error_string(e));
    };
    int64_t wid = (int64_t)(intptr_t)I.video;
    const int we = g_api.set_option(I.h, "wid", MPV_FORMAT_INT64, &wid);
    if (we < 0) I.logf("option wid: %s", g_api.error_string(we));
    // nothing of the user's mpv: no mpv.conf, no scripts, no watch-later resume (SuVtitle keeps its own positions)
    opt("config", "no");
    opt("load-scripts", "no");
    opt("resume-playback", "no");
    opt("save-position-on-quit", "no");
    // HIGH2 (docs\REVIEW_RV-NH.md, measured with a 127.0.0.1 listener): a LOCAL file whose content is a playlist (#EXTM3U in
    // an .mkv), an HLS manifest (in an .mp4), an mpv EDL or a cue sheet made mpv OPEN THE URLS IN IT -- an offline player
    // sending requests a downloaded file chose (a beacon of the user's IP and timing, GETs to the LAN). Never: no
    // reference is followed (playlists, EDL, ordered chapters / linked segments, mov references, archives -- and FFmpeg's
    // nested opens, which mpv refuses under the same option).
    opt("access-references", "no");
    // the contract's set (NATIVE_CONTRACT.md section 3)
    opt("hwdec", "auto-safe");
    opt("vo", "gpu");
    opt("gpu-api", "d3d11");
    opt("d3d11-warp", I.opt.forceWarp ? "yes" : "auto");
    if (I.opt.forceWarp) {
        opt("profile", "fast");
        I.st.warp = true;
    }
    opt("keep-open", "yes");
    opt("osc", "no");
    opt("input-default-bindings", "no");
    opt("input-vo-keyboard", "no");
    opt("sid", "no");                                     // the subtitles are the page's
    opt("audio-display", "no");                           // no cover-art window for a sound file
    // N0's: mpv keeps away from the mouse, the cursor, the focus and the screen text -- the page owns them
    opt("input-builtin-bindings", "no");
    opt("input-cursor", "no");
    opt("input-media-keys", "no");
    opt("cursor-autohide", "no");
    opt("window-dragging", "no");
    opt("focus-on", "never");
    opt("osd-level", "0");
    opt("ytdl", "no");
    opt("idle", "yes");
    // files only: no folder expanded into a playlist, no sibling sound / subtitle file loaded by itself
    opt("directory-mode", "ignore");
    opt("audio-file-auto", "no");
    opt("sub-auto", "no");
    opt("audio-client-name", "SuVtitle");
    if (I.opt.silent) opt("ao", "null");                  // a TEST INSTANCE never makes a sound
    for (const std::string& kv : I.opt.extra) {
        const size_t eq = kv.find('=');
        if (eq != std::string::npos && eq > 0) opt(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
    }
    g_api.request_log_messages(I.h, "info");
    g_api.set_wakeup_callback(I.h, &Impl::wakeup, &I);
    struct Obs { uint64_t id; const char* name; mpv_format f; };
    const Obs obs[] = {
        {P_TIME, "time-pos", MPV_FORMAT_DOUBLE},      {P_DUR, "duration", MPV_FORMAT_DOUBLE},
        {P_PAUSE, "pause", MPV_FORMAT_FLAG},          {P_EOF, "eof-reached", MPV_FORMAT_FLAG},
        {P_SPEED, "speed", MPV_FORMAT_DOUBLE},        {P_VOL, "volume", MPV_FORMAT_DOUBLE},
        {P_MUTE, "mute", MPV_FORMAT_FLAG},            {P_ABA, "ab-loop-a", MPV_FORMAT_STRING},
        {P_ABB, "ab-loop-b", MPV_FORMAT_STRING},      {P_DW, "dwidth", MPV_FORMAT_INT64},
        {P_DH, "dheight", MPV_FORMAT_INT64},          {P_HWDEC, "hwdec-current", MPV_FORMAT_STRING},
        {P_TRACKS, "track-list", MPV_FORMAT_NODE},    {P_SEEKING, "seeking", MPV_FORMAT_FLAG},
        {P_SEEKABLE, "seekable", MPV_FORMAT_FLAG},    {P_IDLE, "idle-active", MPV_FORMAT_FLAG},
        {P_VO, "current-vo", MPV_FORMAT_STRING},      {P_VCODEC, "video-codec", MPV_FORMAT_STRING},
        {P_ACODEC, "audio-codec-name", MPV_FORMAT_STRING},
    };
    for (const Obs& o : obs) {
        const int e = g_api.observe_property(I.h, o.id, o.name, o.f);
        if (e < 0) I.logf("observe %s: %s", o.name, g_api.error_string(e));
    }
    const ULONGLONG ti = nowMs();
    const int ie = g_api.initialize(I.h);
    I.logf("mpv initialised in %llu ms", (unsigned long long)(nowMs() - ti));
    if (ie < 0) {
        I.why = std::string("mpv could not start (") + g_api.error_string(ie) + ")";
        I.logf("%s", I.why.c_str());
        g_api.terminate_destroy(I.h);                     // (no video window exists yet: safe on this thread)
        I.h = nullptr;
        ::DestroyWindow(I.video);
        I.video = nullptr;
        return false;
    }
    for (const char* p : {"mpv-version", "ffmpeg-version"}) {
        char* v = g_api.get_property_string(I.h, p);
        I.logf("%s: %s", p, v ? v : "(unavailable)");
        if (v) g_api.free_mem(v);
    }
    if (!I.opt.forceWarp) {
        auto* ref = new std::shared_ptr<Impl::Probe>(I.probeS);   // (the thread's own reference: LOW7)
        I.probe = ::CreateThread(nullptr, 0, &Impl::probeProc, ref, 0, nullptr);
        if (!I.probe) {
            delete ref;
            I.probeHandled = true;                        // (no probe: d3d11-warp=auto alone)
        }
    } else {
        I.probeHandled = true;
        I.logf("GPU: WARP forced (a test of the software path): profile=fast");
    }
    I.why.clear();
    return true;
}

bool MpvPlayer::available() const { return impl_->h != nullptr && !impl_->shut; }
const std::string& MpvPlayer::why() const { return impl_->why; }
void MpvPlayer::setEventSink(std::function<void(const MpvEvent&)> sink) { impl_->sink = std::move(sink); }
const MpvState& MpvPlayer::state() const { return impl_->st; }

void MpvPlayer::open(const std::wstring& path, uint64_t startMs, int ord, uint64_t gen) {
    Impl& I = *impl_;
    if (!I.h) return;
    // (LOW8: no wait for the GPU probe here -- its answer applies profile=fast whenever it comes)
    // the item's own state: no A-B loop, PAUSED (a <video> given a src does not play until asked: the page plays it)
    I.setStr("ab-loop-a", "no");
    I.setStr("ab-loop-b", "no");
    I.lastAbB = -1;
    I.setFlag("pause", true);
    I.wantOrd = ord;
    // MEDIUM3: this item LOADS from now on -- what is asked meanwhile is held for it; what was held for an earlier one goes
    I.loadingGen = gen;
    I.held = Impl::Act{};
    I.stepAtRestart = 0;
    I.seekSinceEof = false;
    // HIGH1: the sound track ALWAYS goes FILE-LOCAL with the load -- aid=auto when none is asked. A global aid set by a
    // switch during an item (selectAudio) stayed for the next item opened without one (measured: the next file played
    // track 2 -- and an AVI / TS item, whose tracks the page cannot list, kept it for good); a file-local one is undone
    // at the end of its file by mpv itself.
    std::string opts = ord >= 0 ? fmt("aid=%d", ord + 1) : std::string("aid=auto");
    if (startMs > 0) opts += fmt(",start=%.3f", (double)startMs / 1000.0);
    // NAMED arguments: loadfile's positional order changed in mpv 0.38 (an index before the options)
    const std::string url = u8(path);
    const char* keys[4] = {"name", "url", "flags", "options"};
    std::string vals[4] = {"loadfile", url, "replace", opts};
    mpv_node values[4];
    for (int i = 0; i < 4; ++i) {
        values[i].format = MPV_FORMAT_STRING;
        values[i].u.string = const_cast<char*>(vals[i].c_str());
    }
    mpv_node_list list{4, values, const_cast<char**>(keys)};
    mpv_node root;
    root.format = MPV_FORMAT_NODE_MAP;
    root.u.list = &list;
    const uint64_t tag = kReplyLoad | ++I.loadSeq;
    I.replyGen[tag] = gen;
    I.lastOpenGen = gen;
    const int e = g_api.command_node_async(I.h, tag, &root);
    const size_t leaf = path.find_last_of(L"\\/");
    I.logf("open item %llu: %s start=%.3f s ord=%d%s", (unsigned long long)gen,
           u8(leaf == std::wstring::npos ? path : path.substr(leaf + 1)).c_str(), (double)startMs / 1000.0, ord,
           e < 0 ? " -- REFUSED" : "");
    if (e < 0) {
        I.replyGen.erase(tag);
        I.loadingGen = 0;
        MpvEvent x;
        x.kind = MpvEvent::Kind::Error;
        x.gen = gen;
        x.text = endFileSentence(e, g_api.error_string);
        I.pending.push_back(x);
        I.post();
    }
}

void MpvPlayer::stop() {
    Impl& I = *impl_;
    if (!I.h) return;
    I.loadingGen = 0;                                     // (nothing loads any more: nothing held)
    I.held = Impl::Act{};
    I.stepAtRestart = 0;
    I.cmd({"stop"});
}

void MpvPlayer::play() {
    Impl& I = *impl_;
    if (!I.h) return;
    // <video>.play() at the end starts again from the beginning -- unless a seek was asked SINCE that end (L1, page review:
    // mpv still says eof until the seek lands, and a restart from 0 replaced it) or the item is still loading
    if (I.st.eof && !I.seekSinceEof && !I.loadingGen) I.seekTo(0, true);
    I.setFlag("pause", false);
}

void MpvPlayer::pause() { impl_->setFlag("pause", true); }

void MpvPlayer::seek(uint64_t ms, bool exact) {
    if (!impl_->h) return;
    impl_->askSeek((int64_t)std::min<uint64_t>(ms, (uint64_t)INT64_MAX), exact);   // (held while the item loads)
}

void MpvPlayer::setSpeed(double rate) {
    if (!std::isfinite(rate)) return;
    impl_->setDouble("speed", std::min(10.0, std::max(0.1, rate)));
}

void MpvPlayer::setVolume(int pct) { impl_->setDouble("volume", pctToMpv(pct)); }

void MpvPlayer::setMute(bool on) { impl_->setFlag("mute", on); }

void MpvPlayer::setAbLoop(int64_t aMs, int64_t bMs) {
    Impl& I = *impl_;
    if (!I.h) return;
    I.setStr("ab-loop-a", aMs >= 0 ? fmt("%.3f", (double)aMs / 1000.0) : std::string("no"));
    I.setStr("ab-loop-b", bMs >= 0 ? fmt("%.3f", (double)bMs / 1000.0) : std::string("no"));
    // B set AT the playhead (the page's estimate) can land a few ms BEHIND mpv's real position, and mpv loops only when
    // playback reaches B from before it (measured by N0 on Hilda: no loop at all). So setting B jumps to A, as most
    // players do: the loop starts from inside [A, B]. (Only when B CHANGES: the same loop sent again does not jump; held,
    // like any seek, while the item loads.)
    if (aMs >= 0 && bMs > aMs && bMs != I.lastAbB) I.askSeek(aMs, true);
    I.lastAbB = bMs;
}

void MpvPlayer::selectAudio(int ord) {
    Impl& I = *impl_;
    if (!I.h) return;
    I.wantOrd = ord;
    if (ord < 0) { I.setStr("aid", "auto"); return; }
    if (ord < (int)I.audioIds.size()) I.setInt("aid", I.audioIds[(size_t)ord]);
    else if (I.audioIds.empty()) I.setInt("aid", ord + 1);   // (the tracks not known yet: checked when they are)
    else I.logf("sound track %d asked for, the item has %zu: ignored", ord, I.audioIds.size());
}

void MpvPlayer::frameStep(int step) {
    if (!impl_->h || step == 0) return;
    impl_->askFrame(step);                                // (held while the item loads)
}

void MpvPlayer::place(const RECT& clientRect) {
    Impl& I = *impl_;
    I.cssValid = false;                                   // (a rectangle in pixels: the CSS one no longer applies)
    I.fillPending = false;
    I.placeRect(clientRect);
}

void MpvPlayer::placeCss(double x, double y, double w, double h) {
    Impl& I = *impl_;
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) return;
    I.cx = x;
    I.cy = y;
    I.cw = w;
    I.ch = h;
    I.cssValid = true;
    I.fillPending = false;                                // (the page's own word on the full-screen layout)
    I.applyCss();
}

void MpvPlayer::setDpiScale(double scale) {
    Impl& I = *impl_;
    if (!std::isfinite(scale) || scale <= 0.05 || scale > 20.0 || scale == I.scale) return;
    I.scale = scale;
    if (!I.fillPending) I.applyCss();
}

double MpvPlayer::dpiScale() const { return impl_->scale; }

void MpvPlayer::setFullscreenHint(bool on) {
    Impl& I = *impl_;
    if (on == I.fs) return;
    I.fs = on;
    if (on) {
        I.sx = I.cx; I.sy = I.cy; I.sw = I.cw; I.sh = I.ch;
        I.savedValid = I.cssValid;
        I.fillPending = true;                             // the window is resized next: parentResized() fills the client
    } else {
        I.fillPending = false;
        if (I.savedValid) {
            I.cx = I.sx; I.cy = I.sy; I.cw = I.sw; I.ch = I.sh;
            I.cssValid = true;
        }
        I.applyCss();
    }
    I.logf("full screen %s", on ? "on" : "off");
}

void MpvPlayer::parentResized() {
    Impl& I = *impl_;
    if (I.fs && I.fillPending) I.fill();
    else I.applyCss();
}

void MpvPlayer::shutdown() { impl_->shutdownAll(); }

void MpvPlayer::diag(const std::string& line) { impl_->logf("host: %s", line.c_str()); }

HWND MpvPlayer::videoWindow() const { return impl_->video; }

}   // namespace suvmpv
