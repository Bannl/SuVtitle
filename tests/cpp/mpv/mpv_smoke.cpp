// =============================================================================
// SuVtitle tests/cpp/mpv/mpv_smoke.cpp -- N1 (26/10/2026): the native video engine's wrapper, app/MpvPlayer.cpp, against
// the REAL libmpv-2.dll (SUVTITLE_MPV_DIR, the variable the app's build reads too).
//
// THE ABI FIRST (compile time): MpvPlayer.cpp declares the subset of libmpv's C API it uses by itself (the build must not
// need mpv's headers). This file includes the REAL mpv/client.h, then MpvPlayer.cpp, and static_asserts every size,
// offset, enum value and function signature of those declarations against it.
//
// Then, mechanically (each section a FRESH condition, the window on the SECOND screen, never activated, mpv SILENT):
//   [DLL]    (fix round MEDIUM4) a DLL planted in the CURRENT directory loads by name before lockDllSearch(), never after;
//            <exe dir>\mpv\ and System32 still searched
//   [LOAD]   no DLL -> false + a sentence (and native.log says so); the real one -> loaded, the Vulkan loader named
//   [ATTACH] the video window: a DISABLED child (hit tests reach the parent), hidden until placed
//   [PLACE]  the page's CSS rectangle x the DPI scale; w = 0 hides; full screen fills the client at once and the windowed
//            rectangle comes back after it
//   [OPEN]   start-file / file-loaded / duration / size / tracks for THIS item; loaded PAUSED (does not play by itself);
//            the GPU probe says "hardware" on this PC; every event on the UI thread
//   [PLAY]   the clock advances in real time; Time events at most 30 Hz on a 60 fps file (and not starved)
//   [SEEK]   an exact seek lands within one frame (a keyframe seek would not: GOP 1 s); seeking / restart events
//   [FRAME]  frame steps; the 30 Hz clock's TRAILING event (two steps closer than 33 ms: the last one still told)
//   [SPEED]  1.5x: the clock advances 1.5x
//   [AB]     B set BEHIND the playhead (the page's estimate) still loops: setting B jumps to A (N0's fix)
//   [GEN]    an item's events never carry the next item's generation before its start-file, nor the old one after it;
//            open() clears the A-B loop
//   [TRACK]  open(ord) plays that sound track from the first event (no switch); selectAudio by ord; an ord beyond the
//            item's tracks falls back to mpv's own choice (never silence)
//   [AID]    (HIGH1) a track picked DURING an item does not outlive it: the next item opened with no pick = mpv's default
//   [TRACKS] (the contract) Tracks before Loaded; audioList (ids, titles, codec, channels) and hasVideo (cover art is not a
//            picture: an mp3 with a cover, a flac); a NEXT item of the same size keeps its picture size (mpv never says an
//            unchanged dwidth again), a sound file says 0 x 0
//   [HOLD]   (MEDIUM3) a seek / two seeks / setAbLoop's jump / a frame step asked while the item LOADS: done once loaded
//   [STOP]   idle, no item
//   [ERROR]  a file mpv refuses -> an Error event of THAT item with a sentence
//   [EOF]    the end: eof + paused; play() starts again from the beginning (as <video>.play())
//   [EOF2]   (page review L1) at the end, a seek then play(): from the seek, not from 0
//   [VOL]    mute / volume events; the LINEAR percent survives the round trip
//   [NET]    (HIGH2) local files naming a 127.0.0.1 URL (m3u in .mkv, HLS in .mp4, mpv EDL, cue sheet, .pls): no
//            connection; with mpv's default (access-references=yes) the listener does see mpv -- the check is real
//   [WARP]   forced software rendering (as on a PC with no GPU): warp told, the picture still plays
//   [EARLY]  a file opened before the picture was placed; (LOW8) that first open() does not wait for the GPU probe
//   [PCM]    what is HEARD (ao=pcm into a file): sound track ord 1 = 1320 Hz, volume 50 = HALF the amplitude (LINEAR, as
//            <video>.volume -- mpv's own curve is cubic: one eighth)
//   [END]    shutdown while a picture plays and while a file loads: returns at once (a worker terminates, the UI thread
//            answers mpv's window meanwhile); twice = once; calls after it are harmless
//   [LOGCAP] (LOW9) native.log is started again past 1 MB, while it writes and when it opens
//   [CODECS] (only when SUVTITLE_SMOKE_CODECS names a folder of samples) every sample opens and plays
//   [LOG]    native.log has the engine's lines
// Run: build_mpv_smoke.cmd (makes the media with ffmpeg, compiles, runs). Exit 0 = every check passed.
// =============================================================================
#include <windows.h>
#include <winsock2.h>                         // ([NET]'s listener on 127.0.0.1; WIN32_LEAN_AND_MEAN keeps winsock.h out)

#include "mpv/client.h"                       // THE REAL HEADER (SUVTITLE_MPV_DIR\include)
#include "../../../app/MpvPlayer.cpp"         // the product's wrapper, in this translation unit (its abi:: is checked below)

#include <cstddef>
#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

// ---------------------------------------------------------------------------------------------------------------------
// The ABI (compile time)
// ---------------------------------------------------------------------------------------------------------------------
namespace A = suvmpv::abi;
#define SAME_SIZE(T) static_assert(sizeof(::T) == sizeof(A::T) && alignof(::T) == alignof(A::T), #T ": size / alignment")
#define SAME_AT(T, f) static_assert(offsetof(::T, f) == offsetof(A::T, f), #T "." #f ": offset")
#define SAME_VAL(v) static_assert((int)::v == (int)A::v, #v ": value")
SAME_SIZE(mpv_format); SAME_SIZE(mpv_event_id); SAME_SIZE(mpv_log_level); SAME_SIZE(mpv_end_file_reason);
SAME_SIZE(mpv_node); SAME_AT(mpv_node, u); SAME_AT(mpv_node, format);
static_assert(sizeof(((::mpv_node*)nullptr)->u) == sizeof(((A::mpv_node*)nullptr)->u), "mpv_node.u: size");
SAME_SIZE(mpv_node_list); SAME_AT(mpv_node_list, num); SAME_AT(mpv_node_list, values); SAME_AT(mpv_node_list, keys);
SAME_SIZE(mpv_event_property); SAME_AT(mpv_event_property, name); SAME_AT(mpv_event_property, format); SAME_AT(mpv_event_property, data);
SAME_SIZE(mpv_event_log_message); SAME_AT(mpv_event_log_message, prefix); SAME_AT(mpv_event_log_message, level);
SAME_AT(mpv_event_log_message, text); SAME_AT(mpv_event_log_message, log_level);
SAME_SIZE(mpv_event_start_file); SAME_AT(mpv_event_start_file, playlist_entry_id);
SAME_SIZE(mpv_event_end_file); SAME_AT(mpv_event_end_file, reason); SAME_AT(mpv_event_end_file, error);
SAME_AT(mpv_event_end_file, playlist_entry_id); SAME_AT(mpv_event_end_file, playlist_insert_id);
SAME_AT(mpv_event_end_file, playlist_insert_num_entries);
SAME_SIZE(mpv_event_command); SAME_AT(mpv_event_command, result);
SAME_SIZE(mpv_event); SAME_AT(mpv_event, event_id); SAME_AT(mpv_event, error); SAME_AT(mpv_event, reply_userdata); SAME_AT(mpv_event, data);
SAME_VAL(MPV_FORMAT_NONE); SAME_VAL(MPV_FORMAT_STRING); SAME_VAL(MPV_FORMAT_OSD_STRING); SAME_VAL(MPV_FORMAT_FLAG);
SAME_VAL(MPV_FORMAT_INT64); SAME_VAL(MPV_FORMAT_DOUBLE); SAME_VAL(MPV_FORMAT_NODE); SAME_VAL(MPV_FORMAT_NODE_ARRAY);
SAME_VAL(MPV_FORMAT_NODE_MAP); SAME_VAL(MPV_FORMAT_BYTE_ARRAY);
SAME_VAL(MPV_EVENT_NONE); SAME_VAL(MPV_EVENT_SHUTDOWN); SAME_VAL(MPV_EVENT_LOG_MESSAGE); SAME_VAL(MPV_EVENT_GET_PROPERTY_REPLY);
SAME_VAL(MPV_EVENT_SET_PROPERTY_REPLY); SAME_VAL(MPV_EVENT_COMMAND_REPLY); SAME_VAL(MPV_EVENT_START_FILE); SAME_VAL(MPV_EVENT_END_FILE);
SAME_VAL(MPV_EVENT_FILE_LOADED); SAME_VAL(MPV_EVENT_IDLE); SAME_VAL(MPV_EVENT_TICK); SAME_VAL(MPV_EVENT_CLIENT_MESSAGE);
SAME_VAL(MPV_EVENT_VIDEO_RECONFIG); SAME_VAL(MPV_EVENT_AUDIO_RECONFIG); SAME_VAL(MPV_EVENT_SEEK); SAME_VAL(MPV_EVENT_PLAYBACK_RESTART);
SAME_VAL(MPV_EVENT_PROPERTY_CHANGE); SAME_VAL(MPV_EVENT_QUEUE_OVERFLOW); SAME_VAL(MPV_EVENT_HOOK);
SAME_VAL(MPV_LOG_LEVEL_NONE); SAME_VAL(MPV_LOG_LEVEL_FATAL); SAME_VAL(MPV_LOG_LEVEL_ERROR); SAME_VAL(MPV_LOG_LEVEL_WARN);
SAME_VAL(MPV_LOG_LEVEL_INFO); SAME_VAL(MPV_LOG_LEVEL_V); SAME_VAL(MPV_LOG_LEVEL_DEBUG); SAME_VAL(MPV_LOG_LEVEL_TRACE);
SAME_VAL(MPV_END_FILE_REASON_EOF); SAME_VAL(MPV_END_FILE_REASON_STOP); SAME_VAL(MPV_END_FILE_REASON_QUIT);
SAME_VAL(MPV_END_FILE_REASON_ERROR); SAME_VAL(MPV_END_FILE_REASON_REDIRECT);
SAME_VAL(MPV_ERROR_LOADING_FAILED); SAME_VAL(MPV_ERROR_AO_INIT_FAILED); SAME_VAL(MPV_ERROR_VO_INIT_FAILED);
SAME_VAL(MPV_ERROR_NOTHING_TO_PLAY); SAME_VAL(MPV_ERROR_UNKNOWN_FORMAT); SAME_VAL(MPV_ERROR_UNSUPPORTED);
static_assert((MPV_CLIENT_API_VERSION >> 16) == A::kApiMajor, "the client API major version");
// the function signatures: the real type with mpv's own types swapped for MpvPlayer.cpp's must be the member's type
template <class T> struct Tr { using type = T; };
template <> struct Tr<::mpv_handle*> { using type = A::mpv_handle*; };
template <> struct Tr<::mpv_format> { using type = A::mpv_format; };
template <> struct Tr<::mpv_node*> { using type = A::mpv_node*; };
template <> struct Tr<::mpv_event*> { using type = A::mpv_event*; };
template <class R, class... P> struct Tr<R (*)(P...)> { using type = typename Tr<R>::type (*)(typename Tr<P>::type...); };
#define SAME_FN(real, member) static_assert(std::is_same_v<Tr<decltype(&::real)>::type, decltype(A::Api::member)>, #real ": signature")
SAME_FN(mpv_client_api_version, client_api_version); SAME_FN(mpv_error_string, error_string); SAME_FN(mpv_free, free_mem);
SAME_FN(mpv_create, create); SAME_FN(mpv_initialize, initialize); SAME_FN(mpv_terminate_destroy, terminate_destroy);
SAME_FN(mpv_set_option, set_option); SAME_FN(mpv_set_option_string, set_option_string); SAME_FN(mpv_command_async, command_async);
SAME_FN(mpv_command_node_async, command_node_async); SAME_FN(mpv_set_property_async, set_property_async);
SAME_FN(mpv_get_property_string, get_property_string); SAME_FN(mpv_observe_property, observe_property);
SAME_FN(mpv_get_property_async, get_property_async);
SAME_FN(mpv_request_log_messages, request_log_messages); SAME_FN(mpv_wait_event, wait_event);
SAME_FN(mpv_set_wakeup_callback, set_wakeup_callback);

// ---------------------------------------------------------------------------------------------------------------------
// The harness
// ---------------------------------------------------------------------------------------------------------------------
using suvmpv::MpvEvent;
using suvmpv::MpvPlayer;
using K = MpvEvent::Kind;

static int g_pass = 0, g_fail = 0;
static void ok(const char* name, bool cond, const std::string& extra = std::string()) {
    if (cond) { ++g_pass; std::printf("  PASS  %s%s%s\n", name, extra.empty() ? "" : "  (", extra.empty() ? "" : (extra + ")").c_str()); }
    else { ++g_fail; std::printf("  FAIL  %s%s%s\n", name, extra.empty() ? "" : "  -> ", extra.c_str()); }
    std::fflush(stdout);
}
static std::string f(const char* fm, ...) {
    char b[600];
    va_list ap;
    va_start(ap, fm);
    vsnprintf(b, sizeof b, fm, ap);
    va_end(ap);
    return b;
}

struct Rec { MpvEvent e; ULONGLONG t; DWORD tid; };
static DWORD g_uiTid = 0;

// an engine + the record of everything it said
struct Engine {
    MpvPlayer p;
    std::vector<Rec> ev;
    HWND win = nullptr;
    void hook() {
        p.setEventSink([this](const MpvEvent& e) { ev.push_back(Rec{e, ::GetTickCount64(), ::GetCurrentThreadId()}); });
    }
    size_t mark() const { return ev.size(); }
    // the first event of `kind` (and generation, if given) at or after `from`; -1 = none
    long find(K kind, size_t from = 0, long long gen = -1) const {
        for (size_t i = from; i < ev.size(); ++i)
            if (ev[i].e.kind == kind && (gen < 0 || ev[i].e.gen == (uint64_t)gen)) return (long)i;
        return -1;
    }
    size_t count(K kind, size_t from, size_t to) const {
        size_t n = 0;
        for (size_t i = from; i < to && i < ev.size(); ++i) n += ev[i].e.kind == kind;
        return n;
    }
};

static void pump(DWORD ms) {
    const ULONGLONG end = ::GetTickCount64() + ms;
    for (;;) {
        MSG m;
        while (::PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) { ::TranslateMessage(&m); ::DispatchMessageW(&m); }
        const ULONGLONG now = ::GetTickCount64();
        if (now >= end) return;
        ::MsgWaitForMultipleObjects(0, nullptr, FALSE, (DWORD)std::min<ULONGLONG>(10, end - now), QS_ALLINPUT);
    }
}
static bool waitFor(const std::function<bool()>& pred, DWORD ms) {
    const ULONGLONG end = ::GetTickCount64() + ms;
    while (!pred()) {
        if (::GetTickCount64() >= end) return pred();
        pump(5);
    }
    return true;
}

static MpvPlayer* g_resizeTarget = nullptr;
static LRESULT CALLBACK winProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_SIZE && g_resizeTarget) g_resizeTarget->parentResized();
    if (m == WM_ERASEBKGND) {
        RECT rc;
        ::GetClientRect(h, &rc);
        ::FillRect(reinterpret_cast<HDC>(wp), &rc, static_cast<HBRUSH>(::GetStockObject(GRAY_BRUSH)));
        return 1;
    }
    return ::DefWindowProcW(h, m, wp, lp);
}
// a test window on the SECOND screen (the monitor at -1920,0 when there is one), never activated, no taskbar button
static HWND makeWindow(int n, int cw, int ch) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = winProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"SuVtitle.MpvSmoke";
        ::RegisterClassExW(&wc);
        reg = true;
    }
    HMONITOR mon = ::MonitorFromPoint(POINT{-1900, 20}, MONITOR_DEFAULTTONULL);
    if (!mon) mon = ::MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    ::GetMonitorInfoW(mon, &mi);
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, ex = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    RECT r{0, 0, cw, ch};
    ::AdjustWindowRectEx(&r, style, FALSE, ex);
    // the BOTTOM-RIGHT of that screen, room kept for [PLACE]'s 800 x 450: away from where the real-app kits put SuVtitle's
    // test windows (app.txt WINPOS -1880,40), so a smoke and a real-app run never cover each other
    HWND h = ::CreateWindowExW(ex, L"SuVtitle.MpvSmoke", L"SuVtitle mpv_smoke", style, mi.rcWork.right - 840 - n * 16,
                               mi.rcWork.bottom - 520 - n * 16, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                               ::GetModuleHandleW(nullptr), nullptr);
    ::ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}
static void resizeClient(HWND h, int cw, int ch) {
    RECT r{0, 0, cw, ch};
    ::AdjustWindowRectEx(&r, (DWORD)::GetWindowLongPtrW(h, GWL_STYLE), FALSE, (DWORD)::GetWindowLongPtrW(h, GWL_EXSTYLE));
    ::SetWindowPos(h, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
// the video window's rectangle in its parent's client coordinates; empty when hidden
static RECT childRect(MpvPlayer& p, HWND parent) {
    RECT r{};
    HWND v = p.videoWindow();
    if (!v || !::IsWindowVisible(v)) return r;
    ::GetWindowRect(v, &r);
    ::MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&r), 2);
    return r;
}
static std::string rs(const RECT& r) { return f("(%ld,%ld)-(%ld,%ld)", r.left, r.top, r.right, r.bottom); }
static bool same(const RECT& a, long l, long t, long r, long b) { return a.left == l && a.top == t && a.right == r && a.bottom == b; }

static std::wstring envW(const wchar_t* n) {
    wchar_t b[4096] = {};
    const DWORD k = ::GetEnvironmentVariableW(n, b, 4096);
    return (k > 0 && k < 4096) ? std::wstring(b) : std::wstring();
}
static std::wstring here() {
    wchar_t p[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s(p);
    return s.substr(0, s.find_last_of(L"\\/"));
}
static std::string readAll(const std::wstring& path) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::string();
    std::string s;
    char b[65536];
    DWORD rd = 0;
    while (::ReadFile(h, b, sizeof b, &rd, nullptr) && rd) s.append(b, rd);
    ::CloseHandle(h);
    return s;
}
static bool writeAll(const std::wstring& path, const std::string& body) {
    HANDLE h = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wr = 0;
    const BOOL good = ::WriteFile(h, body.data(), (DWORD)body.size(), &wr, nullptr);
    ::CloseHandle(h);
    return good && wr == (DWORD)body.size();
}
static double msNow() {
    LARGE_INTEGER fq, c;
    ::QueryPerformanceFrequency(&fq);
    ::QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)fq.QuadPart;
}
static std::wstring modulePath(HMODULE m) {
    wchar_t p[MAX_PATH] = {};
    if (m) ::GetModuleFileNameW(m, p, MAX_PATH);
    return p;
}
// `file` sits directly in `dir` (case ignored)
static bool underDir(const std::wstring& file, const std::wstring& dir) {
    const size_t s = file.find_last_of(L"\\/");
    return s != std::wstring::npos && s == dir.size()
           && ::CompareStringOrdinal(file.c_str(), (int)s, dir.c_str(), (int)dir.size(), TRUE) == CSTR_EQUAL;
}

// A TCP listener on 127.0.0.1 (a free port): every connection made to it is counted, with its request's first line. A
// connection waits in the backlog until poll() takes it (its request is read then): nothing is ever answered.
struct Listener {
    SOCKET s = INVALID_SOCKET;
    int port = 0;
    std::vector<std::string> hits;
    bool start() {
        WSADATA wd;
        if (::WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int len = (int)sizeof a;
        if (::bind(s, reinterpret_cast<sockaddr*>(&a), (int)sizeof a) != 0 || ::listen(s, 16) != 0
            || ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0)
            return false;
        port = ntohs(a.sin_port);
        u_long nb = 1;
        ::ioctlsocket(s, FIONBIO, &nb);
        return true;
    }
    void poll() {
        for (;;) {
            SOCKET c = ::accept(s, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            u_long nb = 0;
            ::ioctlsocket(c, FIONBIO, &nb);
            DWORD to = 300;
            ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), (int)sizeof to);
            char b[400] = {};
            const int n = ::recv(c, b, (int)sizeof b - 1, 0);
            std::string req = n > 0 ? std::string(b, (size_t)n) : std::string("(no request)");
            const size_t eol = req.find_first_of("\r\n");
            if (eol != std::string::npos) req.resize(eol);
            hits.push_back(req);
            ::closesocket(c);
        }
    }
    void stop() {
        if (s != INVALID_SOCKET) ::closesocket(s);
        s = INVALID_SOCKET;
        ::WSACleanup();
    }
};

// what was HEARD: a WAV (ao=pcm, s16 mono): the peak and the frequency (zero crossings) between `from` and `to` seconds
struct Heard { bool ok = false; int rate = 0, ch = 0, bits = 0, fmt = 0; double secs = 0, peak = 0, hz = 0; };
static Heard analyse(const std::string& w, double from, double to) {
    Heard r;
    if (w.size() < 44 || w.compare(0, 4, "RIFF") != 0 || w.compare(8, 4, "WAVE") != 0) return r;
    size_t at = 12, data = 0, len = 0;
    while (at + 8 <= w.size()) {
        const std::string id = w.substr(at, 4);
        const uint32_t n = *reinterpret_cast<const uint32_t*>(&w[at + 4]);
        if (id == "fmt ") {
            r.fmt = *reinterpret_cast<const uint16_t*>(&w[at + 8]);
            r.ch = *reinterpret_cast<const uint16_t*>(&w[at + 10]);
            r.rate = (int)*reinterpret_cast<const uint32_t*>(&w[at + 12]);
            r.bits = *reinterpret_cast<const uint16_t*>(&w[at + 22]);
        } else if (id == "data") {
            data = at + 8;
            len = std::min<size_t>(n, w.size() - data);
            break;
        }
        at += 8 + n + (n & 1);
    }
    if (!data || r.bits != 16 || r.ch != 1 || r.rate <= 0) return r;
    const size_t total = len / 2;
    r.secs = (double)total / r.rate;
    const size_t a = (size_t)(from * r.rate), b = std::min(total, (size_t)(to * r.rate));
    if (b <= a + 100) return r;
    const int16_t* s = reinterpret_cast<const int16_t*>(&w[data]);
    long crossings = 0;
    for (size_t i = a; i < b; ++i) {
        r.peak = std::max(r.peak, std::fabs((double)s[i]));
        if (i > a && ((s[i - 1] < 0) != (s[i] < 0))) ++crossings;
    }
    r.hz = crossings / 2.0 / ((double)(b - a) / r.rate);
    r.ok = true;
    return r;
}

int main() {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_uiTid = ::GetCurrentThreadId();
    const std::wstring mpvDir = envW(L"SUVTITLE_MPV_DIR");
    const std::wstring work = here();                             // tests\cpp\mpv\work
    const std::wstring media = work + L"\\media\\main60.mkv", notMedia = work + L"\\media\\notmedia.mkv";
    const std::wstring cover = work + L"\\media\\cover.mp3", tone = work + L"\\media\\tone.flac";
    if (mpvDir.empty() || ::GetFileAttributesW((mpvDir + L"\\libmpv-2.dll").c_str()) == INVALID_FILE_ATTRIBUTES
        || ::GetFileAttributesW(media.c_str()) == INVALID_FILE_ATTRIBUTES || ::GetFileAttributesW(cover.c_str()) == INVALID_FILE_ATTRIBUTES
        || ::GetFileAttributesW(tone.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::printf("mpv_smoke: NOT RUN -- SUVTITLE_MPV_DIR must hold libmpv-2.dll, and work\\media\\ main60.mkv, cover.mp3 and "
                    "tone.flac must exist (build_mpv_smoke.cmd makes them)\n");
        return 2;
    }
    std::printf("mpv_smoke: libmpv from %ls\n", mpvDir.c_str());

    // ---- [DLL] (fix round MEDIUM4) the DLL search LOCKED, as wWinMain's first call does -- for the rest of this run too --
    {
        const std::wstring cwd = work + L"\\plantcwd", mpvSub = work + L"\\mpv";
        ::CreateDirectoryW(cwd.c_str(), nullptr);
        ::CreateDirectoryW(mpvSub.c_str(), nullptr);
        wchar_t sys[MAX_PATH] = {};
        ::GetSystemDirectoryW(sys, MAX_PATH);
        const std::wstring harmless = std::wstring(sys) + L"\\msimg32.dll";       // a tiny system DLL, under planted names
        const bool planted = ::CopyFileW(harmless.c_str(), (cwd + L"\\zzn1plant1.dll").c_str(), FALSE)
                             && ::CopyFileW(harmless.c_str(), (cwd + L"\\zzn1plant2.dll").c_str(), FALSE)
                             && ::CopyFileW(harmless.c_str(), (mpvSub + L"\\zzn1mpvdir.dll").c_str(), FALSE);
        wchar_t old[MAX_PATH] = {};
        ::GetCurrentDirectoryW(MAX_PATH, old);
        ::SetCurrentDirectoryW(cwd.c_str());                   // (Explorer's --play: the CWD is the film's folder)
        HMODULE before = ::LoadLibraryW(L"zzn1plant1.dll");
        const std::wstring beforeFrom = modulePath(before);
        if (before) ::FreeLibrary(before);
        const bool locked = MpvPlayer::lockDllSearch();
        HMODULE after = ::LoadLibraryW(L"zzn1plant2.dll");
        const DWORD afterErr = after ? 0 : ::GetLastError();
        if (after) ::FreeLibrary(after);
        HMODULE viaMpv = ::LoadLibraryW(L"zzn1mpvdir.dll");
        const std::wstring viaFrom = modulePath(viaMpv);
        if (viaMpv) ::FreeLibrary(viaMpv);
        HMODULE d3d = ::LoadLibraryW(L"d3d11.dll");
        ::SetCurrentDirectoryW(old);
        ok("[DLL] the planted CWD: BEFORE the lock, a bare-name load comes from the CWD (the hole the review found)",
           planted && before && underDir(beforeFrom, cwd), f("%ls", beforeFrom.c_str()));
        ok("[DLL] after MpvPlayer::lockDllSearch(): the same kind of DLL in the CWD is NOT loaded", locked && !after,
           f("locked %d, after %p, error %lu", (int)locked, (void*)after, afterErr));
        ok("[DLL] ...<exe dir>\\mpv\\ IS searched (the bundle's libmpv folder), and System32 by name", viaMpv && underDir(viaFrom, mpvSub)
           && d3d != nullptr, f("%ls", viaFrom.c_str()));
    }

    // ---- [LOAD] --------------------------------------------------------------------------------------------------------
    {
        MpvPlayer none;
        suvmpv::MpvOptions no;
        no.logPath = work + L"\\native_noload.log";
        ::DeleteFileW(no.logPath.c_str());
        none.setOptions(no);
        std::string why;
        const std::wstring empty = work + L"\\no_mpv_here";
        ::CreateDirectoryW(empty.c_str(), nullptr);
        const bool l = none.load(empty, why);
        ok("[LOAD] no libmpv-2.dll: load() is false with a sentence naming it", !l && why.find("libmpv-2.dll") != std::string::npos
           && !none.loaded(), why);
        ok("[LOAD] ...and nothing is available (attach refuses)", !none.attach(::GetDesktopWindow()) && !none.available()
           && !none.why().empty(), none.why());
        none.shutdown();
        const std::string nl = readAll(no.logPath);
        ok("[LOAD] ...and native.log SAYS so (fix round LOW9: the log opens before load(), a PC without the DLL is diagnosable)",
           nl.find("NOT available -- libmpv-2.dll is not installed") != std::string::npos, nl.substr(0, 160));
    }
    Engine P;
    suvmpv::MpvOptions po;
    po.silent = true;
    po.logPath = work + L"\\native_smoke.log";
    ::DeleteFileW(po.logPath.c_str());
    P.p.setOptions(po);                                    // (BEFORE load, as main.cpp: load() opens the log)
    {
        std::string why;
        const bool l = P.p.load(mpvDir, why);
        ok("[LOAD] the real libmpv-2.dll loads (client API 2.x, every export found)", l && P.p.loaded() && why.empty(), why);
        if (!l) { std::printf("mpv_smoke: cannot go on\n"); return 1; }
        const std::string pl = readAll(po.logPath);
        ok("[LOAD] native.log's first line names the Vulkan loader in use (N3: this PC's own, in System32)",
           pl.find("---- SuVtitle native engine: libmpv client API 2.") != std::string::npos
           && pl.find("Vulkan loader: the system's (System32)") != std::string::npos, pl.substr(0, 220));
    }

    // ---- [ATTACH] ------------------------------------------------------------------------------------------------------
    P.win = makeWindow(0, 640, 360);
    P.hook();
    g_resizeTarget = &P.p;
    const bool att = P.p.attach(P.win);
    HWND vid = P.p.videoWindow();
    ok("[ATTACH] attach(): mpv created and initialised, available, no reason given", att && P.p.available() && P.p.why().empty(), P.p.why());
    ok("[ATTACH] the video window is a DISABLED child of the player's window, hidden until placed",
       vid && ::IsChild(P.win, vid) && (::GetWindowLongPtrW(vid, GWL_STYLE) & WS_DISABLED) && !::IsWindowVisible(vid));
    if (!att) return 1;

    // ---- [PLACE] -------------------------------------------------------------------------------------------------------
    {
        P.p.setDpiScale(1.5);
        P.p.placeCss(10, 20, 300, 100);
        const RECT a = childRect(P.p, P.win);
        ok("[PLACE] the page's CSS rectangle x 1.5: (15,30)-(465,180), shown", same(a, 15, 30, 465, 180), rs(a));
        // hit tests: the picture never takes the mouse (a disabled child is skipped -- with what mpv makes inside it)
        HWND hit = ::ChildWindowFromPointEx(P.win, POINT{200, 100}, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED | CWP_SKIPTRANSPARENT);
        ok("[PLACE] a point on the picture belongs to the PARENT for hit-testing", hit == P.win);
        P.p.placeCss(0, 0, 0, 0);
        ok("[PLACE] w = 0 hides the picture (a sound file, no item)", !::IsWindowVisible(vid));
        P.p.placeCss(10, 20, 300, 100);
        // full screen: the page's stage is the whole page there -- the client is filled at once, before the page says so
        P.p.setFullscreenHint(true);
        resizeClient(P.win, 800, 450);
        pump(50);
        const RECT b = childRect(P.p, P.win);
        ok("[PLACE] full screen: the picture fills the resized client at once", same(b, 0, 0, 800, 450), rs(b));
        P.p.placeCss(0, 0, 800 / 1.5, 450 / 1.5);       // the page's full-screen rectangle arrives
        P.p.setFullscreenHint(false);
        resizeClient(P.win, 640, 360);
        pump(50);
        const RECT c = childRect(P.p, P.win);
        ok("[PLACE] out of full screen: the WINDOWED rectangle comes back (not the full-screen one)", same(c, 15, 30, 465, 180), rs(c));
        P.p.setDpiScale(1.0);
        const RECT d = childRect(P.p, P.win);
        ok("[PLACE] a new DPI scale re-applies the rectangle: (10,20)-(310,120)", same(d, 10, 20, 310, 120), rs(d));
        P.p.placeCss(0, 0, 640, 360);
    }

    // ---- [OPEN] --------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 0, -1, 1);
        const bool loaded = waitFor([&] { return P.find(K::Loaded, m0, 1) >= 0 && P.p.state().durMs > 0 && P.p.state().w > 0
                                                 && P.p.state().audioTracks > 0; }, 8000);
        const auto& s = P.p.state();
        ok("[OPEN] start-file then file-loaded, both for item 1", loaded && P.find(K::Start, m0, 1) >= 0
           && P.find(K::Start, m0, 1) < P.find(K::Loaded, m0, 1));
        ok("[OPEN] duration 10 s, picture 320 x 180, 2 sound tracks, the first heard", std::llabs(s.durMs - 10000) <= 100
           && s.w == 320 && s.h == 180 && s.audioTracks == 2 && s.ord == 0 && s.gen == 1,
           f("dur %lld %dx%d tracks %d ord %d gen %llu", (long long)s.durMs, s.w, s.h, s.audioTracks, s.ord, (unsigned long long)s.gen));
        pump(700);
        ok("[OPEN] loaded PAUSED: it does not play by itself (the page plays it)", P.p.state().paused && P.p.state().timeMs < 100,
           f("paused %d t %lld", (int)P.p.state().paused, (long long)P.p.state().timeMs));
        waitFor([&] { return !P.p.state().hwdec.empty(); }, 3000);
        ok("[OPEN] the decoder is told (hwdec)", !P.p.state().hwdec.empty(), P.p.state().hwdec);
        ok("[OPEN] the GPU probe: this PC has a usable GPU -> no WARP, no profile=fast", !P.p.state().warp);
        bool ui = true;
        for (const Rec& r : P.ev) ui = ui && r.tid == g_uiTid;
        ok("[OPEN] every event reached the sink on the UI thread", ui && !P.ev.empty(), f("%zu events", P.ev.size()));
    }

    // ---- [PLAY] --------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        const int64_t t0 = P.p.state().timeMs;
        P.p.play();
        waitFor([&] { return !P.p.state().paused; }, 2000);
        pump(300);                                        // (the first frames)
        const size_t m1 = P.mark();
        const int64_t a = P.p.state().timeMs;
        const ULONGLONG w0 = ::GetTickCount64();
        pump(2000);
        const int64_t b = P.p.state().timeMs;
        const double wall = (double)(::GetTickCount64() - w0);
        const size_t m2 = P.mark();
        const size_t n = P.count(K::Time, m1, m2);
        ok("[PLAY] a Pause(false) event", P.find(K::Pause, m0) >= 0 && !P.p.state().paused);
        ok("[PLAY] the clock advances in real time (2 s of wall clock)", std::fabs((double)(b - a) - wall) <= 250,
           f("media %lld ms in %.0f ms (from %lld)", (long long)(b - a), wall, (long long)t0));
        const double hz = n * 1000.0 / wall;
        // (<= 33: one event per 33 ms + the trailing one and the window's two edges -- 31.5 Hz was measured twice in 42 runs
        // on 02/11 with a game on the GPU; uncoalesced, a 60 fps file gives 60)
        ok("[PLAY] Time events at most ~30 Hz on a 60 fps file, and not starved (>= 20 Hz)", hz <= 33.0 && hz >= 20.0, f("%.1f Hz", hz));
    }

    // ---- [SEEK] --------------------------------------------------------------------------------------------------------
    {
        P.p.pause();
        waitFor([&] { return P.p.state().paused; }, 2000);
        const size_t m0 = P.mark();
        P.p.seek(1234, true);
        waitFor([&] { return P.find(K::Restart, m0) >= 0 && !P.p.state().seeking; }, 4000);
        pump(150);
        const int64_t t = P.p.state().timeMs;
        ok("[SEEK] an EXACT seek lands within one frame (60 fps; GOP 1 s: a keyframe seek would say 1000)", std::llabs(t - 1234) <= 17,
           f("%lld", (long long)t));
        ok("[SEEK] seeking(true) was told, then a restart", P.find(K::Seeking, m0) >= 0 && P.find(K::Restart, m0) >= 0);
        const size_t m1 = P.mark();
        P.p.seek(5000, false);
        waitFor([&] { return P.find(K::Restart, m1) >= 0 && !P.p.state().seeking; }, 4000);
        pump(150);
        ok("[SEEK] a keyframe seek onto a keyframe", std::llabs(P.p.state().timeMs - 5000) <= 17, f("%lld", (long long)P.p.state().timeMs));
    }

    // ---- [FRAME] -------------------------------------------------------------------------------------------------------
    {
        const int64_t t0 = P.p.state().timeMs;
        P.p.frameStep(1);
        P.p.frameStep(1);
        pump(600);
        const int64_t t1 = P.p.state().timeMs;
        long last = -1;
        for (size_t i = 0; i < P.ev.size(); ++i) if (P.ev[i].e.kind == K::Time) last = (long)i;
        const int64_t told = last >= 0 ? P.ev[(size_t)last].e.ms : -1;
        ok("[FRAME] two frame steps forward: +2 frames (33 ms), still paused", std::llabs((t1 - t0) - 33) <= 4 && P.p.state().paused,
           f("%lld -> %lld", (long long)t0, (long long)t1));
        ok("[FRAME] the 30 Hz clock's TRAILING event: the last Time told is the position now", told == t1,
           f("told %lld, now %lld", (long long)told, (long long)t1));
        P.p.frameStep(-1);
        pump(600);
        ok("[FRAME] one frame step back: -1 frame", std::llabs((t1 - P.p.state().timeMs) - 17) <= 4,
           f("%lld -> %lld", (long long)t1, (long long)P.p.state().timeMs));
    }

    // ---- [SPEED] -------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        P.p.seek(1000, true);
        P.p.setSpeed(1.5);
        P.p.play();
        waitFor([&] { return !P.p.state().paused && P.p.state().speed == 1.5; }, 2000);
        pump(400);
        const int64_t a = P.p.state().timeMs;
        const ULONGLONG w0 = ::GetTickCount64();
        pump(1500);
        const double ratio = (double)(P.p.state().timeMs - a) / (double)(::GetTickCount64() - w0);
        const long se = P.find(K::Speed, m0);
        ok("[SPEED] a Speed(1.5) event, and the clock runs 1.5x", se >= 0 && P.ev[(size_t)se].e.value == 1.5 && std::fabs(ratio - 1.5) <= 0.15,
           f("ratio %.2f", ratio));
        P.p.setSpeed(1.0);
    }

    // ---- [AB] ----------------------------------------------------------------------------------------------------------
    {
        P.p.seek(500, true);
        P.p.play();
        waitFor([&] { return P.p.state().timeMs >= 2000; }, 4000);
        const int64_t b = P.p.state().timeMs - 30;        // B a little BEHIND mpv's position (the page's estimate)
        const size_t m0 = P.mark();
        P.p.setAbLoop(1000, b);
        bool wrapped = false, beyond = false;
        int64_t prev = P.p.state().timeMs, low = INT64_MAX;
        const ULONGLONG end = ::GetTickCount64() + 2500;
        while (::GetTickCount64() < end) {
            pump(10);
            const int64_t t = P.p.state().timeMs;
            if (t < prev - 300) wrapped = true;
            if (wrapped) { low = std::min(low, t); if (t > b + 150) beyond = true; }
            prev = t;
        }
        const long ab = P.find(K::AbLoop, m0);
        ok("[AB] the loop is told (A 1000, B behind the playhead)", ab >= 0 && P.p.state().abA == 1000 && P.p.state().abB == b,
           f("A %lld B %lld", (long long)P.p.state().abA, (long long)P.p.state().abB));
        ok("[AB] setting B jumps to A: it LOOPS (never past B afterwards)", wrapped && !beyond && std::llabs(low - 1000) <= 150,
           f("wrapped %d beyond %d low %lld", (int)wrapped, (int)beyond, (long long)(low == INT64_MAX ? -1 : low)));
        P.p.setAbLoop(-1, -1);
        waitFor([&] { return P.p.state().abA < 0 && P.p.state().abB < 0; }, 2000);
        ok("[AB] cleared: both points off", P.p.state().abA < 0 && P.p.state().abB < 0);
    }

    // ---- [GEN] ---------------------------------------------------------------------------------------------------------
    {
        P.p.setAbLoop(3000, 6000);                        // (open() clears it: that change belongs to the OLD item)
        waitFor([&] { return P.p.state().abB == 6000; }, 2000);
        const size_t m0 = P.mark();
        P.p.open(media, 0, -1, 2);
        waitFor([&] { return P.find(K::Loaded, m0, 2) >= 0 && P.p.state().durMs > 0; }, 8000);
        pump(400);
        const long st = P.find(K::Start, m0, 2);
        bool before = true, after = true;
        std::string bad;
        for (size_t i = m0; i < P.ev.size(); ++i) {
            const uint64_t g = P.ev[i].e.gen;
            if (st < 0 || (long)i < st) { if (g == 2) { before = false; bad += f(" kind%d@%zu", (int)P.ev[i].e.kind, i); } }
            else if (g == 1) { after = false; bad += f(" kind%d@%zu(gen1)", (int)P.ev[i].e.kind, i); }
        }
        ok("[GEN] the old item's last events (its A-B cleared, its end) never carry the NEW generation", st >= 0 && before, bad);
        ok("[GEN] after the new item's start, no event carries the OLD generation", after, bad);
        ok("[GEN] open() cleared the A-B loop", P.p.state().abA < 0 && P.p.state().abB < 0 && P.p.state().gen == 2,
           f("A %lld B %lld", (long long)P.p.state().abA, (long long)P.p.state().abB));
    }

    // ---- [TRACK] -------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 0, 1, 3);
        waitFor([&] { return P.find(K::Loaded, m0, 3) >= 0 && P.p.state().ord >= 0; }, 8000);
        pump(300);
        const long first = P.find(K::Track, m0, 3);
        ok("[TRACK] open(ord 1): sound track 1 from the item's FIRST Track event (no switch at the start)",
           P.p.state().ord == 1 && first >= 0 && P.ev[(size_t)first].e.ord == 1,
           f("ord %d, first Track event ord %d", P.p.state().ord, first >= 0 ? P.ev[(size_t)first].e.ord : -9));
        const size_t m1 = P.mark();
        P.p.selectAudio(0);
        waitFor([&] { return P.p.state().ord == 0; }, 2000);
        P.p.selectAudio(1);
        waitFor([&] { return P.p.state().ord == 1 && P.find(K::Track, m1) >= 0; }, 2000);
        ok("[TRACK] selectAudio(0) then selectAudio(1): Track events, track 1 heard", P.p.state().ord == 1 && P.count(K::Track, m1, P.mark()) >= 2,
           f("ord %d", P.p.state().ord));
        const size_t m2 = P.mark();
        P.p.open(media, 0, 5, 4);
        waitFor([&] { return P.find(K::Loaded, m2, 4) >= 0 && P.p.state().ord >= 0; }, 8000);
        ok("[TRACK] an ord beyond the item's tracks: mpv's own choice, never silence", P.p.state().ord == 0, f("ord %d", P.p.state().ord));
    }

    // ---- [AID] (fix round HIGH1) a sound track picked DURING an item never outlives it ---------------------------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 0, -1, 31);
        waitFor([&] { return P.find(K::Loaded, m0, 31) >= 0 && P.p.state().ord >= 0; }, 8000);
        pump(200);
        const int first = P.p.state().ord;
        P.p.selectAudio(1);                               // (the user's pick, in the track menu)
        waitFor([&] { return P.p.state().ord == 1; }, 3000);
        const int picked = P.p.state().ord;
        const size_t m1 = P.mark();
        P.p.open(media, 0, -1, 32);                       // the next item, no pick (an AVI / TS: the page knows no list)
        waitFor([&] { return P.find(K::Loaded, m1, 32) >= 0 && P.p.state().ord >= 0; }, 8000);
        pump(300);
        ok("[AID] item A: the user picks track 1 during it; item B, opened with no pick: mpv's DEFAULT track again (aid=auto "
           "goes FILE-LOCAL with every load)", first == 0 && picked == 1 && P.p.state().ord == 0,
           f("A %d -> %d, then B %d", first, picked, P.p.state().ord));
    }

    // ---- [TRACKS] (fix round: the contract's track list -- Tracks / audioList / hasVideo) --------------------------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 0, -1, 33);
        waitFor([&] { return P.find(K::Loaded, m0, 33) >= 0; }, 8000);
        const long tr = P.find(K::Tracks, m0, 33), ld = P.find(K::Loaded, m0, 33);
        const auto& L = P.p.state().audioList;
        std::string got;
        for (const auto& a : L)
            got += f(" [id %lld '%s' %s %dch%s]", (long long)a.id, a.title.c_str(), a.codec.c_str(), a.channels, a.isDefault ? " default" : "");
        ok("[TRACKS] a Tracks event of the item BEFORE its Loaded (the list is known when Loaded comes)", tr >= 0 && ld >= 0 && tr < ld,
           f("Tracks at %ld, Loaded at %ld", tr, ld));
        ok("[TRACKS] audioList at Loaded: the 2 sound tracks in ord order, as the file says (ids 1 / 2, A440 / B1320, flac, mono)",
           L.size() == 2 && (int)L.size() == P.p.state().audioTracks && L[0].id == 1 && L[1].id == 2 && L[0].title == "A440"
           && L[1].title == "B1320" && L[0].codec == "flac" && L[1].codec == "flac" && L[0].channels == 1 && L[1].channels == 1, got);
        ok("[TRACKS] hasVideo at Loaded: the file has a picture", ld >= 0 && P.p.state().hasVideo);
        // the Nth opening of the SAME file: mpv does not say again a property whose value did not change (dwidth, seekable)
        // -- the state must still hold this item's picture size and seekability
        waitFor([&] { return P.p.state().w > 0 && P.p.state().seekable; }, 3000);
        ok("[TRACKS] a NEXT item of the same size: its picture size and seekability are known (320 x 180, seekable)",
           P.p.state().w == 320 && P.p.state().h == 180 && P.p.state().seekable,
           f("%dx%d seekable %d", P.p.state().w, P.p.state().h, (int)P.p.state().seekable));
        // a sound file with a COVER (an attached picture: mpv lists it as a video track, "albumart") -- no picture to show
        const size_t m1 = P.mark();
        P.p.open(cover, 0, -1, 34);
        waitFor([&] { return P.find(K::Loaded, m1, 34) >= 0; }, 8000);
        const auto& C = P.p.state().audioList;
        ok("[TRACKS] an mp3 with a cover: hasVideo FALSE (cover art is not a picture), one sound track (mp3)",
           P.find(K::Loaded, m1, 34) >= 0 && !P.p.state().hasVideo && C.size() == 1 && C[0].codec == "mp3",
           f("loaded %d hasVideo %d tracks %zu codec %s", (int)(P.find(K::Loaded, m1, 34) >= 0), (int)P.p.state().hasVideo, C.size(),
             C.empty() ? "-" : C[0].codec.c_str()));
        waitFor([&] { return P.p.state().w == 0 && P.p.state().h == 0; }, 3000);
        ok("[TRACKS] ...and no picture size (0 x 0: the previous item's 320 x 180 does not stay)", P.p.state().w == 0 && P.p.state().h == 0,
           f("%dx%d", P.p.state().w, P.p.state().h));
        const size_t m2 = P.mark();
        P.p.open(tone, 0, -1, 35);
        waitFor([&] { return P.find(K::Loaded, m2, 35) >= 0; }, 8000);
        ok("[TRACKS] a flac: hasVideo false, one sound track; the list was reset from the previous item",
           P.find(K::Loaded, m2, 35) >= 0 && !P.p.state().hasVideo && P.p.state().audioList.size() == 1
           && P.p.state().audioList[0].codec == "flac", f("hasVideo %d tracks %zu", (int)P.p.state().hasVideo, P.p.state().audioList.size()));
    }

    // ---- [HOLD] (fix round MEDIUM3) asked while the item LOADS (mpv refuses a seek then): kept, done once it is loaded --
    {
        auto settled = [&](size_t from, uint64_t gen, int64_t target) {   // (not "near": a Windows macro)
            return waitFor([&] { return P.find(K::Loaded, from, (long long)gen) >= 0 && !P.p.state().seeking
                                        && std::llabs(P.p.state().timeMs - target) <= 40; }, 6000);
        };
        const size_t m0 = P.mark();
        P.p.open(media, 0, -1, 36);
        P.p.seek(4000, true);                             // (at once: mpv is opening the file)
        settled(m0, 36, 4000);
        pump(200);
        ok("[HOLD] a seek asked right after open() lands once the item is loaded (exact: 4000)",
           std::llabs(P.p.state().timeMs - 4000) <= 17 && P.p.state().paused, f("t %lld", (long long)P.p.state().timeMs));
        const size_t m1 = P.mark();
        P.p.open(media, 0, -1, 37);
        P.p.seek(4000, true);
        P.p.seek(6000, true);                             // (the LAST one wins)
        settled(m1, 37, 6000);
        pump(200);
        ok("[HOLD] two seeks while it loads: the LAST one (6000)", std::llabs(P.p.state().timeMs - 6000) <= 17,
           f("t %lld", (long long)P.p.state().timeMs));
        const size_t m2 = P.mark();
        P.p.open(media, 0, -1, 38);
        P.p.setAbLoop(3000, 5000);                        // (setting B jumps to A: that jump is held too)
        settled(m2, 38, 3000);
        pump(200);
        ok("[HOLD] setAbLoop while it loads: the loop is set AND its jump to A is done once loaded",
           P.p.state().abA == 3000 && P.p.state().abB == 5000 && std::llabs(P.p.state().timeMs - 3000) <= 17,
           f("A %lld B %lld t %lld", (long long)P.p.state().abA, (long long)P.p.state().abB, (long long)P.p.state().timeMs));
        P.p.setAbLoop(-1, -1);
        const size_t m5 = P.mark();
        P.p.open(media, 2000, -1, 45);                    // an item opened at a START position (a film resumed)
        P.p.seek(6000, true);
        settled(m5, 45, 6000);
        pump(200);
        ok("[HOLD] ...also over the item's own start position (opened at 2 s, a seek to 6 s while it loads: 6 s)",
           std::llabs(P.p.state().timeMs - 6000) <= 17, f("t %lld", (long long)P.p.state().timeMs));
        const size_t m3 = P.mark();
        P.p.open(media, 2000, -1, 39);
        P.p.frameStep(-1);                                // (one frame BACK from the start: 1983)
        settled(m3, 39, 1983);
        pump(400);
        ok("[HOLD] a frame step BACK while it loads: done once the item stands at its start (one frame back from 2 s)",
           std::llabs(P.p.state().timeMs - 1983) <= 4 && P.p.state().paused, f("t %lld", (long long)P.p.state().timeMs));
        const size_t m6 = P.mark();
        P.p.open(media, 2000, -1, 46);
        P.p.frameStep(1);                                 // (one frame FORWARD from the start: 2017)
        settled(m6, 46, 2017);
        pump(400);
        ok("[HOLD] a frame step FORWARD while it loads: one frame past the 2 s start, paused",
           std::llabs(P.p.state().timeMs - 2017) <= 4 && P.p.state().paused, f("t %lld", (long long)P.p.state().timeMs));
        // an item that ENDS before it loads (another open replaces it): what it held is dropped, never done on the next one
        const size_t m4 = P.mark();
        P.p.open(media, 0, -1, 40);
        P.p.seek(7000, true);
        P.p.open(media, 0, -1, 41);
        waitFor([&] { return P.find(K::Loaded, m4, 41) >= 0; }, 8000);
        pump(600);
        ok("[HOLD] a seek held by an item replaced before it loaded is NOT done on the next one",
           P.p.state().gen == 41 && P.p.state().timeMs < 100, f("gen %llu t %lld", (unsigned long long)P.p.state().gen,
                                                                 (long long)P.p.state().timeMs));
    }

    // ---- [STOP] --------------------------------------------------------------------------------------------------------
    {
        P.p.stop();
        waitFor([&] { return P.p.state().idle && P.p.state().gen == 0; }, 3000);
        ok("[STOP] stop(): idle, no item (generation 0), no duration", P.p.state().idle && P.p.state().gen == 0 && P.p.state().durMs < 0,
           f("idle %d gen %llu dur %lld", (int)P.p.state().idle, (unsigned long long)P.p.state().gen, (long long)P.p.state().durMs));
        P.p.seek(1000, true);                             // (no file: mpv refuses it -- the refusal is LOGGED by name: [LOG])
        pump(300);
    }

    // ---- [ERROR] -------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        P.p.open(notMedia, 0, -1, 5);
        waitFor([&] { return P.find(K::Error, m0, 5) >= 0; }, 8000);
        const long e = P.find(K::Error, m0, 5);
        ok("[ERROR] a file mpv refuses: an Error event of THAT item, with a sentence",
           e >= 0 && P.ev[(size_t)e].e.text.rfind("The video engine could not play this file", 0) == 0,
           e >= 0 ? P.ev[(size_t)e].e.text : std::string("no Error event"));
    }

    // ---- [EOF] ---------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 9500, -1, 6);
        waitFor([&] { return P.find(K::Loaded, m0, 6) >= 0 && P.p.state().timeMs >= 9400; }, 8000);
        ok("[EOF] open at 9.5 s (startMs)", std::llabs(P.p.state().timeMs - 9500) <= 40, f("%lld", (long long)P.p.state().timeMs));
        P.p.play();
        waitFor([&] { return P.p.state().eof; }, 4000);
        pump(200);
        ok("[EOF] the end: eof, paused on the last frame", P.p.state().eof && P.p.state().paused && P.find(K::Eof, m0, 6) >= 0,
           f("eof %d paused %d t %lld Eof event %ld (any gen %ld)", (int)P.p.state().eof, (int)P.p.state().paused,
             (long long)P.p.state().timeMs, P.find(K::Eof, m0, 6), P.find(K::Eof, m0)));
        P.p.play();
        waitFor([&] { return !P.p.state().paused && P.p.state().timeMs >= 0 && P.p.state().timeMs < 1500; }, 3000);
        ok("[EOF] play() at the end starts again from the beginning (as <video>.play())",
           !P.p.state().paused && !P.p.state().eof && P.p.state().timeMs < 1500,
           f("paused %d eof %d t %lld", (int)P.p.state().paused, (int)P.p.state().eof, (long long)P.p.state().timeMs));
        P.p.pause();
    }

    // ---- [EOF2] (page review L1, the host half) AT the end, a seek then play(): from the seek, never from 0 --------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 9500, -1, 42);
        waitFor([&] { return P.find(K::Loaded, m0, 42) >= 0 && P.p.state().timeMs >= 9400; }, 8000);
        P.p.play();
        waitFor([&] { return P.p.state().eof; }, 4000);
        pump(200);
        const bool atEnd = P.p.state().eof;
        P.p.seek(5000, true);
        P.p.play();                                       // (mpv still says eof here: the seek has not landed)
        int64_t low = INT64_MAX;
        const ULONGLONG end = ::GetTickCount64() + 1500;
        while (::GetTickCount64() < end) {
            pump(10);
            if (P.p.state().timeMs >= 0) low = std::min(low, P.p.state().timeMs);
        }
        ok("[EOF2] at the end, seek(5000) then play(): it plays on from 5 s (play() did not restart from 0 over the seek)",
           atEnd && !P.p.state().paused && P.p.state().timeMs >= 5000 && P.p.state().timeMs < 7500 && low >= 4900,
           f("at end %d, then t %lld (lowest %lld) paused %d", (int)atEnd, (long long)P.p.state().timeMs,
             (long long)(low == INT64_MAX ? -1 : low), (int)P.p.state().paused));
        // and the end reached AGAIN after that seek: play() restarts from 0 again (a new end, nothing sought since)
        P.p.seek(9700, true);
        waitFor([&] { return P.p.state().eof; }, 4000);
        pump(200);
        P.p.play();
        waitFor([&] { return !P.p.state().paused && P.p.state().timeMs >= 0 && P.p.state().timeMs < 1500; }, 3000);
        ok("[EOF2] a NEW end after that: play() restarts from the beginning again", P.p.state().timeMs < 1500,
           f("t %lld", (long long)P.p.state().timeMs));
        P.p.pause();
    }

    // ---- [VOL] ---------------------------------------------------------------------------------------------------------
    {
        const size_t m0 = P.mark();
        P.p.setMute(true);
        waitFor([&] { return P.p.state().muted; }, 2000);
        const long v = P.find(K::Volume, m0);
        ok("[VOL] mute: a Volume event saying muted", v >= 0 && P.ev[(size_t)v].e.flag && P.p.state().muted);
        P.p.setMute(false);
        const size_t m1 = P.mark();
        P.p.setVolume(50);
        waitFor([&] { return P.p.state().volPct == 50; }, 2000);
        const long v2 = P.find(K::Volume, m1);
        ok("[VOL] volume 50 %: told back as 50 (the LINEAR percent survives mpv's cubic scale)",
           P.p.state().volPct == 50 && v2 >= 0 && P.ev[(size_t)v2].e.value == 50.0, f("%d", P.p.state().volPct));
        P.p.setVolume(100);
    }

    // ---- [NET] (fix round HIGH2) a LOCAL file never makes mpv open a URL it names -----------------------------------------
    {
        Listener L;
        const bool up = L.start();
        const std::wstring nd = work + L"\\net";
        ::CreateDirectoryW(nd.c_str(), nullptr);
        const std::string url = f("http://127.0.0.1:%d/", L.port);
        struct Case { const wchar_t* name; std::string body; };
        const Case cases[] = {
            {L"m3u.mkv", "#EXTM3U\n#EXTINF:10,x\n" + url + "from_m3u.mkv\n"},
            {L"hls.mp4", "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n#EXTINF:10.0,\n" + url + "from_hls.ts\n#EXT-X-ENDLIST\n"},
            {L"edl.mkv", "# mpv EDL v0\n" + url + "from_edl.mkv,0,5\n"},
            {L"sheet.cue", "FILE \"" + url + "from_cue.wav\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n"},
            {L"list.pls", "[playlist]\nFile1=" + url + "from_pls.mkv\nNumberOfEntries=1\nVersion=2\n"},
        };
        // one engine's run over every case: the connections each one made
        auto sweep = [&](Engine& E, uint64_t gen0) {
            std::string got;
            uint64_t g = gen0;
            for (const Case& c : cases) {
                const std::wstring p = nd + L"\\" + c.name;
                writeAll(p, c.body);
                const size_t m0 = E.mark(), h0 = L.hits.size();
                E.p.open(p, 0, -1, ++g);
                waitFor([&] { L.poll(); return E.find(K::Error, m0, (long long)g) >= 0 || E.find(K::Loaded, m0, (long long)g) >= 0
                                               || L.hits.size() > h0; }, 4000);
                const ULONGLONG until = ::GetTickCount64() + 1200;       // (a connection could follow the end)
                while (::GetTickCount64() < until) { pump(20); L.poll(); }
                got += f(" %ls:%zu", c.name, L.hits.size() - h0);
            }
            return got;
        };
        const std::string got = sweep(P, 50);
        ok("[NET] m3u in an .mkv, HLS in an .mp4, an mpv EDL, a cue sheet, a .pls -- all naming 127.0.0.1: NO connection",
           up && L.hits.empty(), got + (L.hits.empty() ? std::string() : " first: " + L.hits[0]));
        // mpv's DEFAULT (access-references=yes, given last through extra): the listener does see mpv -- the check above is real
        Engine S;
        S.win = makeWindow(4, 320, 180);
        suvmpv::MpvOptions so;
        so.silent = true;
        so.extra = {"access-references=yes"};
        S.p.setOptions(so);
        S.hook();
        std::string why;
        S.p.load(mpvDir, why);
        S.p.attach(S.win);
        const size_t h0 = L.hits.size();
        const std::string seen = sweep(S, 60);
        ok("[NET] (the check is real) with mpv's default, access-references=yes, the same files DO connect",
           L.hits.size() > h0, seen + (L.hits.size() > h0 ? " first: " + L.hits[h0] : std::string()));
        S.p.shutdown();
        ::DestroyWindow(S.win);
        L.stop();
    }

    // ---- [END] while a picture plays (mpv's window exists: its end SENDS messages to our thread) ----------------------
    {
        const size_t m0 = P.mark();
        P.p.open(media, 2000, -1, 7);
        P.p.play();
        waitFor([&] { return P.find(K::Restart, m0, 7) >= 0 && !P.p.state().paused && P.p.state().timeMs > 2300; }, 6000);
        HWND mpvWin = ::GetWindow(P.p.videoWindow(), GW_CHILD);
        DWORD mpvTid = mpvWin ? ::GetWindowThreadProcessId(mpvWin, nullptr) : 0;
        ok("[END] mpv's own window lives inside ours, on ANOTHER thread (the cross-thread child)", mpvWin && mpvTid && mpvTid != g_uiTid);
        const ULONGLONG t0 = ::GetTickCount64();
        P.p.shutdown();
        const ULONGLONG spent = ::GetTickCount64() - t0;
        ok("[END] shutdown() while it plays returns at once (no deadlock on mpv's window)", spent < 3000 && !P.p.available(),
           f("%llu ms", (unsigned long long)spent));
        P.p.shutdown();
        P.p.open(media, 0, -1, 8);
        P.p.play();
        P.p.seek(100, true);
        P.p.placeCss(0, 0, 10, 10);
        ok("[END] twice = once; every call after it is harmless; why() says it ended", !P.p.available() && !P.p.why().empty(), P.p.why());
        g_resizeTarget = nullptr;
    }

    // ---- [WARP] --------------------------------------------------------------------------------------------------------
    {
        Engine W;
        W.win = makeWindow(1, 480, 270);
        suvmpv::MpvOptions wo;
        wo.silent = true;
        wo.forceWarp = true;
        W.p.setOptions(wo);
        W.hook();
        std::string why;
        W.p.load(mpvDir, why);
        const bool a = W.p.attach(W.win);
        W.p.placeCss(0, 0, 480, 270);
        const size_t m0 = W.mark();
        W.p.open(media, 1000, -1, 1);
        W.p.play();
        waitFor([&] { return W.p.state().timeMs > 1800 && !W.p.state().hwdec.empty(); }, 8000);
        const long hw = W.find(K::Hwdec, m0);
        ok("[WARP] forced software rendering: warp told (state + the Hwdec event), the picture still plays",
           a && W.p.state().warp && hw >= 0 && W.ev[(size_t)hw].e.flag && W.p.state().timeMs > 1800,
           f("warp %d t %lld hwdec %s", (int)W.p.state().warp, (long long)W.p.state().timeMs, W.p.state().hwdec.c_str()));
        // (mid-load: open, then at once the end -- mpv is opening the file meanwhile)
        W.p.open(media, 0, -1, 2);
        const ULONGLONG t0 = ::GetTickCount64();
        W.p.shutdown();
        const ULONGLONG spent = ::GetTickCount64() - t0;
        ok("[END] shutdown() while a file LOADS returns at once", spent < 3000 && !W.p.available(), f("%llu ms", (unsigned long long)spent));
        ::DestroyWindow(W.win);
    }

    // ---- [EARLY] a file opened BEFORE the page placed the picture (--play at the start: the bridge opens mpv as soon as the
    // item opens, the page's first nativeRect comes later) -- mpv's video output starts in a window that was never placed ----
    for (const bool warp : {false, true}) {
        const char* how = warp ? "[EARLY/WARP]" : "[EARLY/GPU]";
        Engine E;
        E.win = makeWindow(3, 480, 270);
        suvmpv::MpvOptions eo;
        eo.silent = true;
        eo.forceWarp = warp;
        eo.logPath = work + (warp ? L"\\native_early_warp.log" : L"\\native_early.log");
        ::DeleteFileW(eo.logPath.c_str());
        E.p.setOptions(eo);
        E.hook();
        std::string why;
        E.p.load(mpvDir, why);
        E.p.attach(E.win);
        const size_t m0 = E.mark();
        const double o0 = msNow();
        E.p.open(media, 1000, -1, 1);                     // (at once after attach(): the GPU probe has not answered)
        const double openMs = msNow() - o0;
        if (!warp) {
            const std::string el = readAll(eo.logPath);
            const size_t at = el.find("open item 1:"), gpu = el.find("GPU: ");
            // the probe answers on this thread's queue: had open() waited for it, its "GPU:" line would come first
            ok("[EARLY/GPU] (LOW8) the first open() right after attach() does not wait for the GPU probe",
               at != std::string::npos && (gpu == std::string::npos || gpu > at) && openMs < 30.0,
               f("open() took %.1f ms; GPU line %s", openMs, gpu == std::string::npos ? "not yet" : (gpu > at ? "after" : "BEFORE")));
        }
        E.p.play();
        waitFor([&] { return E.p.state().timeMs > 1800 && E.p.state().w > 0; }, 8000);
        const bool noError = E.find(K::Error, m0) < 0;
        ok((std::string(how) + " opened before any placement: it loads and plays, no error (the video output started in the "
            "never-placed window)").c_str(),
           noError && E.p.state().timeMs > 1800 && E.p.state().w == 320 && !E.p.state().hwdec.empty() && E.p.state().warp == warp,
           f("t %lld w %d hwdec %s warp %d error %s", (long long)E.p.state().timeMs, E.p.state().w, E.p.state().hwdec.c_str(),
             (int)E.p.state().warp, noError ? "none" : E.ev[(size_t)E.find(K::Error, m0)].e.text.c_str()));
        E.p.placeCss(0, 0, 480, 270);
        pump(400);
        HWND mw = ::GetWindow(E.p.videoWindow(), GW_CHILD);
        RECT mr{};
        if (mw) { ::GetClientRect(mw, &mr); }
        ok((std::string(how) + " ...then placed: the picture window shows, mpv's own window fills it").c_str(),
           mw && ::IsWindowVisible(E.p.videoWindow()) && mr.right == 480 && mr.bottom == 270, f("mpv window %ldx%ld", mr.right, mr.bottom));
        E.p.shutdown();
        ::DestroyWindow(E.win);
    }

    // ---- [PCM] what is heard ---------------------------------------------------------------------------------------------
    {
        Engine C;
        C.win = makeWindow(2, 320, 180);
        const std::wstring wav = work + L"\\heard.wav";
        ::DeleteFileW(wav.c_str());
        suvmpv::MpvOptions co;
        co.silent = true;                                 // (ao=null first: if ao=pcm were refused, still never a sound)
        co.extra = {"ao=pcm", "ao-pcm-file=" + suvmpv::u8(wav), "ao-pcm-waveheader=yes", "audio-format=s16", "audio-channels=mono",
                    "audio-samplerate=48000"};
        C.p.setOptions(co);
        C.hook();
        std::string why;
        C.p.load(mpvDir, why);
        C.p.attach(C.win);
        C.p.placeCss(0, 0, 320, 180);
        C.p.setVolume(50);
        const size_t m0 = C.mark();
        C.p.open(media, 7000, 1, 1);
        waitFor([&] { return C.find(K::Loaded, m0, 1) >= 0 && C.p.state().ord == 1; }, 8000);
        C.p.play();
        waitFor([&] { return C.p.state().eof; }, 8000);
        C.p.shutdown();                                   // (the AO closed: the WAV is complete)
        ::DestroyWindow(C.win);
        const Heard h = analyse(readAll(wav), 0.5, 2.0);
        ok("[PCM] the sound came out (ao=pcm, s16 mono 48 kHz)", h.ok && h.secs >= 2.5, f("%.2f s fmt %d %d ch %d bits %d Hz", h.secs, h.fmt, h.ch, h.bits, h.rate));
        ok("[PCM] open(ord 1) is HEARD: the second track's 1320 Hz (the first one is 440 Hz)", h.ok && std::fabs(h.hz - 1320) <= 30,
           f("%.1f Hz", h.hz));
        ok("[PCM] volume 50 % = HALF the amplitude (LINEAR, like <video>.volume; mpv alone would give an eighth)",
           h.ok && std::fabs(h.peak / 16383.0 - 0.5) <= 0.03, f("peak %.0f = %.3f of the source's", h.peak, h.peak / 16383.0));
    }

    // ---- [LOGCAP] (fix round LOW9) native.log never grows past 1 MB, even within one long session -----------------------
    {
        const std::wstring capLog = work + L"\\native_cap.log";
        const std::string filler(1024 * 1024 - 60, 'x');  // just under the cap: opened for appending
        writeAll(capLog, filler);
        suvmpv::MpvOptions lo;
        lo.logPath = capLog;
        std::string why;
        {
            MpvPlayer Lp;
            Lp.setOptions(lo);
            Lp.load(mpvDir, why);                         // (the log opens -- libmpv is loaded already: nothing written yet)
            const size_t before = readAll(capLog).size();
            Lp.diag(std::string(200, 'y'));               // this line would take it past 1 MB
            const std::string after = readAll(capLog);
            ok("[LOGCAP] a line that would take native.log past 1 MB starts it again (the cap holds WHILE it writes)",
               before == filler.size() && after.size() < 4096 && after.find("(native.log started again") != std::string::npos
               && after.find(std::string(200, 'y')) != std::string::npos && after.find("xxxxxxxx") == std::string::npos,
               f("%zu -> %zu bytes", before, after.size()));
            Lp.shutdown();
        }
        writeAll(capLog, std::string(1024 * 1024 + 10, 'x'));   // over the cap when it opens
        {
            MpvPlayer Lq;
            Lq.setOptions(lo);
            Lq.load(mpvDir, why);
            Lq.diag("hello");
            const std::string q = readAll(capLog);
            ok("[LOGCAP] a native.log already over 1 MB is started again when it opens",
               q.size() < 4096 && q.find("(native.log started again") != std::string::npos && q.find("host: hello") != std::string::npos,
               f("%zu bytes", q.size()));
            Lq.shutdown();
        }
    }

    // ---- [CODECS] (only when SUVTITLE_SMOKE_CODECS names a folder of samples) every sample opens and plays --------------
    {
        const std::wstring cdir = envW(L"SUVTITLE_SMOKE_CODECS");
        if (cdir.empty()) {
            std::printf("  (skipped [CODECS]: SUVTITLE_SMOKE_CODECS is not set)\n");
        } else {
            std::vector<std::wstring> files;
            WIN32_FIND_DATAW fd{};
            HANDLE fh = ::FindFirstFileW((cdir + L"\\*").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(fd.cFileName);
                } while (::FindNextFileW(fh, &fd));
                ::FindClose(fh);
            }
            Engine C;
            C.win = makeWindow(5, 640, 360);
            suvmpv::MpvOptions co;
            co.silent = true;
            C.p.setOptions(co);
            C.hook();
            std::string why;
            C.p.load(mpvDir, why);
            C.p.attach(C.win);
            C.p.placeCss(0, 0, 640, 360);
            std::string bad;
            uint64_t g = 100;
            for (const std::wstring& n : files) {
                const size_t m0 = C.mark();
                C.p.open(cdir + L"\\" + n, 0, -1, ++g);
                C.p.play();
                waitFor([&] { return C.find(K::Error, m0, (long long)g) >= 0
                                     || (C.find(K::Loaded, m0, (long long)g) >= 0 && C.p.state().timeMs > 1500); }, 8000);
                const auto& s = C.p.state();
                const bool good = C.find(K::Error, m0, (long long)g) < 0 && s.timeMs > 1500;
                std::printf("    %-28ls %s  t %lld  %dx%d  hwdec %s  sound tracks %d%s\n", n.c_str(), good ? "plays" : "FAILS",
                            (long long)s.timeMs, s.w, s.h, s.hwdec.c_str(), s.audioTracks, s.hasVideo ? "" : "  (no picture)");
                if (!good) bad += f(" %ls", n.c_str());
            }
            ok("[CODECS] every sample opens and plays (access-references=no and the fix round broke none)", !files.empty() && bad.empty(),
               f("%zu files;%s", files.size(), bad.empty() ? " all play" : bad.c_str()));
            C.p.shutdown();
            ::DestroyWindow(C.win);
        }
    }

    // ---- [LOG] ---------------------------------------------------------------------------------------------------------
    {
        const std::string log = readAll(po.logPath);
        const char* must[] = {"libmpv client API 2.", "mpv-version: ", "GPU: ", "open item 1:", "start-file: item 1", "end-file: item",
                              "mpv terminated", "Vulkan loader: ", "the seek asked while item 36 loaded: 4.000 s, done now",
                              "the frame step asked while item 39 loaded: done now", "seek refused by mpv: "};
        std::string missing;
        for (const char* m : must) if (log.find(m) == std::string::npos) missing += std::string(" [") + m + "]";
        ok("[LOG] native.log has the engine's lines (versions, the GPU, opens, starts, ends, the termination)", missing.empty(), missing);
        // open(ord 1) reached mpv as aid=2 WITH the load: mpv already plays it, nothing to put right -- no second selection
        // (setting aid again re-starts the sound chain; mpv's track list can say "none selected" at file-loaded: 26/10)
        ok("[LOG] no needless second selection of the track mpv already plays (open(ord) was right the first time)",
           log.find("sound track 1 is mpv's id") == std::string::npos && log.find("no sound track playing") == std::string::npos);
    }

    ::DestroyWindow(P.win);
    std::printf("mpv_smoke: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
