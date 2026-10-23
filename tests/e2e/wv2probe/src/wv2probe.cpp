// =============================================================================
// wv2probe -- a WebView2 TEST INSTRUMENT for AudioPB (not product code).
//
// Hosts one WebView2 in a window that never takes focus, loads --url, optionally
// maps virtual hosts to folders (SetVirtualHostNameToFolderMapping) and/or serves
// hosts from WebResourceRequested with Cache-Control: no-store (exactly like the
// product's https://patchbus.local/), replays a JSONL "feed" into the page
// (PostWebMessageAsJson + a few host commands) and appends everything the page
// posts (WebMessageReceived) plus host events (navigation, console, JS exceptions,
// Log domain, optional Network domain, screenshots, exit) to a JSONL "out" file.
//
// Never hangs: a UI-thread timer ends the run at --timeout (exit 2) and a watchdog
// thread terminates the process 20 s later if the UI thread itself is stuck (3).
//
// Build: build.cmd (cl + the WebView2 SDK copied into .\sdk). Usage: README.txt.
//
// PORTED to SuVtitle (S-WEB, 2026-10-23) from AudioPB's scratchpad/filemode_2026-10-10/wv2probe_tool/src/
// wv2probe.cpp -- the frozen copy of the 641f1aa era (SuVtitle/scratchpad/from_audiopb_641f1aa/), UNCHANGED but
// for this note: the instrument is product-agnostic (its --serve / --map / --feed say what to load). SuVtitle's
// kits serve https://suvtitle.local/ from app\web and map *.suvtitle-media.example the way SuVtitle.exe does.
// =============================================================================
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <wrl.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <string>
#include <vector>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"

#pragma comment(lib, "WebView2LoaderStatic.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "version.lib")

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

static const char* kToolVersion = "1.0";
static const char* kSdkVersion  = "1.0.2592.51";

// ---------------------------------------------------------------------------
// String helpers
// ---------------------------------------------------------------------------
static std::string u8(const wchar_t* w) {
    if (!w || !*w) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return std::string();
    std::string s((size_t)n - 1, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}
static std::string u8(const std::wstring& w) { return u8(w.c_str()); }
static std::wstring wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static bool ieq(const std::wstring& a, const wchar_t* b) { return ::_wcsicmp(a.c_str(), b) == 0; }

static std::string jq(const std::string& s) {           // JSON string literal
    std::string o;
    o.reserve(s.size() + 2);
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        case '\b': o += "\\b";  break;
        case '\f': o += "\\f";  break;
        default:
            if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
            else o += (char)c;
        }
    }
    o += '"';
    return o;
}
static std::string jn(double v) {                        // JSON number
    if (!std::isfinite(v)) return "null";
    char b[64];
    if (std::fabs(v) < 1e15 && std::fabs(v - (double)std::llround(v)) < 1e-9)
        std::snprintf(b, sizeof b, "%lld", (long long)std::llround(v));
    else
        std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
static std::string hrs(HRESULT hr) { char b[16]; std::snprintf(b, sizeof b, "0x%08lX", (unsigned long)hr); return b; }
static std::string cap(const std::string& s, size_t n = 4000) { return s.size() <= n ? s : s.substr(0, n) + "...[truncated]"; }

// ---------------------------------------------------------------------------
// Minimal JSON (parse + dump). Enough for feed lines and CDP payloads.
// ---------------------------------------------------------------------------
namespace json {
struct V {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;                                       // string value, or raw number text
    std::vector<V> a;
    std::vector<std::pair<std::string, V>> o;
    const V* get(const char* k) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == k) return &kv.second;
        return nullptr;
    }
    std::string str(const char* k, const std::string& def = std::string()) const {
        const V* v = get(k); return (v && v->t == Str) ? v->s : def;
    }
    double num(const char* k, double def = 0) const {
        const V* v = get(k); return (v && v->t == Num) ? v->n : def;
    }
};

struct Parser {
    const char* p; const char* e;
    bool ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p; return p < e; }
    bool lit(const char* w) {
        const size_t n = std::strlen(w);
        if ((size_t)(e - p) < n || std::memcmp(p, w, n) != 0) return false;
        p += n; return true;
    }
    static void put(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
        else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        ++p;
        while (p < e) {
            const char c = *p++;
            if (c == '"') return true;
            if ((unsigned char)c < 0x20) return false;
            if (c != '\\') { out += c; continue; }
            if (p >= e) return false;
            const char x = *p++;
            switch (x) {
            case '"': out += '"'; break;   case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break;  case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;  case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else { put(out, 0xFFFD); cp = lo; }
                    } else cp = 0xFFFD;
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) cp = 0xFFFD;
                put(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool val(V& v, int depth) {
        if (depth > 256 || !ws()) return false;
        const char c = *p;
        if (c == '{') {
            ++p; v.t = V::Obj;
            if (!ws()) return false;
            if (*p == '}') { ++p; return true; }
            for (;;) {
                if (!ws()) return false;
                std::string k;
                if (!str(k)) return false;
                if (!ws() || *p != ':') return false;
                ++p;
                V child;
                if (!val(child, depth + 1)) return false;
                v.o.emplace_back(std::move(k), std::move(child));
                if (!ws()) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++p; v.t = V::Arr;
            if (!ws()) return false;
            if (*p == ']') { ++p; return true; }
            for (;;) {
                V child;
                if (!val(child, depth + 1)) return false;
                v.a.push_back(std::move(child));
                if (!ws()) return false;
                if (*p == ',') { ++p; continue; }
                if (*p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { v.t = V::Str; return str(v.s); }
        if (lit("true"))  { v.t = V::Bool; v.b = true;  return true; }
        if (lit("false")) { v.t = V::Bool; v.b = false; return true; }
        if (lit("null"))  { v.t = V::Null; return true; }
        const char* s0 = p;
        if (p < e && *p == '-') ++p;
        const char* d0 = p;
        while (p < e && *p >= '0' && *p <= '9') ++p;
        if (p == d0) return false;
        if (p < e && *p == '.') { ++p; const char* f0 = p; while (p < e && *p >= '0' && *p <= '9') ++p; if (p == f0) return false; }
        if (p < e && (*p == 'e' || *p == 'E')) {
            ++p;
            if (p < e && (*p == '+' || *p == '-')) ++p;
            const char* x0 = p; while (p < e && *p >= '0' && *p <= '9') ++p;
            if (p == x0) return false;
        }
        v.t = V::Num; v.s.assign(s0, p); v.n = std::strtod(v.s.c_str(), nullptr);
        return true;
    }
};
static bool parse(const std::string& in, V& out) {
    Parser ps{ in.data(), in.data() + in.size() };
    if (!ps.val(out, 0)) return false;
    ps.ws();
    return ps.p == ps.e;
}
static bool valid(const std::string& in) { V tmp; return parse(in, tmp); }
static std::string dump(const V& v) {
    switch (v.t) {
    case V::Null: return "null";
    case V::Bool: return v.b ? "true" : "false";
    case V::Num:  return v.s.empty() ? jn(v.n) : v.s;
    case V::Str:  return jq(v.s);
    case V::Arr: {
        std::string o = "[";
        for (size_t i = 0; i < v.a.size(); ++i) { if (i) o += ','; o += dump(v.a[i]); }
        return o + "]";
    }
    case V::Obj: {
        std::string o = "{";
        for (size_t i = 0; i < v.o.size(); ++i) { if (i) o += ','; o += jq(v.o[i].first); o += ':'; o += dump(v.o[i].second); }
        return o + "}";
    }
    }
    return "null";
}
}  // namespace json

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct MapSpec   { std::wstring host, folder; COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND kind; std::string kindName; };
struct ServeSpec { std::wstring host, folder; };

struct Options {
    std::wstring url, feed, out, shot, udf, browserArgs;
    std::vector<MapSpec> maps;
    std::vector<ServeSpec> serves;
    std::vector<std::string> exitOn;
    DWORD timeoutMs = 60000;
    int w = 1280, h = 800;
    // Default: inside the user's SECOND screen (\\.\DISPLAY12 = (-1920,0) 1920x1080, left of the
    // primary). If no monitor is there at run time -> bottom-right of the primary work area.
    int posX = -1880, posY = 40;
    double scale = 1.0;                                  // <= 0: follow the monitor
    bool net = false, sound = false, productSettings = false, quiet = false, logServe = false;
    bool noSmartScreen = false;                          // ICoreWebView2Settings8::IsReputationCheckingRequired = FALSE
} g_opt;

// ---------------------------------------------------------------------------
// Global state (single UI thread; only the watchdog runs elsewhere)
// ---------------------------------------------------------------------------
enum : UINT_PTR { T_TIMEOUT = 1, T_SLEEP, T_WAITFOR, T_SHOT, T_EXITGRACE, T_ENVRETRY };
constexpr UINT WM_APP_FEED = WM_APP + 1;

HWND                              g_hwnd = nullptr;
ComPtr<ICoreWebView2Environment>  g_env;
ComPtr<ICoreWebView2Controller>   g_ctl;
ComPtr<ICoreWebView2>             g_wv;
std::vector<ComPtr<ICoreWebView2DevToolsProtocolEventReceiver>> g_receivers;
int                               g_envAttempts = 0;

CRITICAL_SECTION g_outLock;
HANDLE           g_outFile = INVALID_HANDLE_VALUE;
HANDLE           g_stdout  = nullptr;
LARGE_INTEGER    g_qpf{}, g_qp0{};

std::vector<std::string> g_msgs;                         // every page message, raw JSON

struct FeedLine { int no; std::string text; };
std::vector<FeedLine> g_feed;
size_t      g_feedPos = 0;
bool        g_feedStarted = false, g_feedBusy = false, g_feedDone = false;
size_t      g_prevMark = 0;                              // g_msgs.size() when the previous line ran
bool        g_waiting = false;
std::string g_waitSub;
int         g_waitLine = 0;
double      g_waitT0 = 0;
bool        g_navWaitFeed = false;

bool   g_firstNavDone = false;
double g_navT0 = 0;

struct Shot {
    bool active = false; int id = 0; int line = 0; double t0 = 0;
    std::wstring path; ComPtr<IStream> stream; std::function<void(bool)> done;
} g_shot;
int g_shotSeq = 0;

bool g_exiting = false, g_exitFinished = false;
int  g_exitCode = 0;
bool g_anchorBottomRight = false;                        // true when we fell back to the primary's corner

static double nowMs() {
    LARGE_INTEGER q; ::QueryPerformanceCounter(&q);
    return (double)(q.QuadPart - g_qp0.QuadPart) * 1000.0 / (double)g_qpf.QuadPart;
}

// ---------------------------------------------------------------------------
// Output: one JSON object per line, to --out (append) and echoed to stdout
// ---------------------------------------------------------------------------
static void writeAll(HANDLE h, const std::string& s) {
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    ::WriteFile(h, s.data(), (DWORD)s.size(), &wr, nullptr);
}
static void emitLine(const std::string& line, bool echo) {
    ::EnterCriticalSection(&g_outLock);
    const std::string l = line + "\n";
    if (g_outFile != INVALID_HANDLE_VALUE) writeAll(g_outFile, l);
    if (echo) writeAll(g_stdout, l);
    ::LeaveCriticalSection(&g_outLock);
}

struct Ev {                                              // host event builder
    std::string s;
    explicit Ev(const char* kind) { s = "{\"__host\":" + jq(kind) + ",\"t\":" + jn(std::round(nowMs() * 10) / 10); }
    Ev& kv(const char* k, const std::string& v)  { s += ',' + jq(k) + ':' + jq(v); return *this; }
    Ev& kv(const char* k, const char* v)         { return kv(k, std::string(v ? v : "")); }
    Ev& kv(const char* k, const std::wstring& v) { return kv(k, u8(v)); }
    Ev& kn(const char* k, double v)              { s += ',' + jq(k) + ':' + jn(v); return *this; }
    Ev& kb(const char* k, bool v)                { s += ',' + jq(k) + ':' + (v ? "true" : "false"); return *this; }
    Ev& khr(const char* k, HRESULT hr)           { return kv(k, hrs(hr)); }
    Ev& kraw(const char* k, const std::string& raw) {   // embeds raw JSON (quoted if invalid)
        s += ',' + jq(k) + ':' + ((!raw.empty() && json::valid(raw)) ? raw : jq(raw));
        return *this;
    }
    std::string str() const { return s + "}"; }
};
static void hostEvent(const Ev& ev, bool important = false) { emitLine(ev.str(), important || !g_opt.quiet); }

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
static void beginExit(int code, const std::string& reason);
static void finishExit();
static void feedNext();
static void createEnv();

// ---------------------------------------------------------------------------
// Focus self-check: the instrument must never take the foreground
// ---------------------------------------------------------------------------
static void checkFocus(const char* when) {
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    if (fg) ::GetWindowThreadProcessId(fg, &pid);
    const bool ours = fg && (fg == g_hwnd || ::GetAncestor(fg, GA_ROOT) == g_hwnd || pid == ::GetCurrentProcessId());
    hostEvent(Ev("focus").kv("when", when).kb("stolen", ours), ours);
}

// ---------------------------------------------------------------------------
// Screenshots (CapturePreview -> PNG)
// ---------------------------------------------------------------------------
static void shotFinished(int id, HRESULT r, bool timedOut) {
    if (!g_shot.active || g_shot.id != id) return;
    ::KillTimer(g_hwnd, T_SHOT);
    g_shot.active = false;
    g_shot.stream.Reset();                               // closes the file (unless WebView2 still holds it)
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    long long bytes = 0;
    if (::GetFileAttributesExW(g_shot.path.c_str(), GetFileExInfoStandard, &fa))
        bytes = ((long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    const bool ok = SUCCEEDED(r) && !timedOut && bytes > 0;
    Ev ev("shot");
    ev.kn("line", g_shot.line).kv("path", g_shot.path).kb("ok", ok).kn("bytes", (double)bytes).kn("ms", nowMs() - g_shot.t0);
    if (FAILED(r)) ev.khr("hr", r);
    if (timedOut) ev.kb("timeout", true);
    hostEvent(ev, !ok);
    auto done = std::move(g_shot.done);
    g_shot.done = nullptr;
    if (done) done(ok);
}

static void takeShot(const std::wstring& pathIn, int line, std::function<void(bool)> done) {
    if (g_shot.active) {                                 // only on exit: drop the pending one
        ::KillTimer(g_hwnd, T_SHOT);
        g_shot.active = false;
        g_shot.stream.Reset();
        g_shot.done = nullptr;
        hostEvent(Ev("shot").kn("line", g_shot.line).kv("path", g_shot.path).kb("ok", false).kv("err", "abandoned (exit)"), true);
    }
    wchar_t full[2048] = {};
    if (!::GetFullPathNameW(pathIn.c_str(), 2048, full, nullptr)) ::wcsncpy_s(full, pathIn.c_str(), _TRUNCATE);
    std::wstring dir = full;
    const size_t sl = dir.find_last_of(L"\\/");
    if (sl != std::wstring::npos) { dir.resize(sl); ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); }
    ComPtr<IStream> st;
    HRESULT hr = g_wv ? ::SHCreateStreamOnFileEx(full, STGM_CREATE | STGM_WRITE | STGM_SHARE_DENY_WRITE,
                                                 FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &st)
                      : E_FAIL;
    if (FAILED(hr) || !st) {
        hostEvent(Ev("shot").kn("line", line).kv("path", full).kb("ok", false).khr("hr", hr), true);
        if (done) done(false);
        return;
    }
    const int id = ++g_shotSeq;
    g_shot.active = true; g_shot.id = id; g_shot.line = line; g_shot.t0 = nowMs();
    g_shot.path = full; g_shot.stream = st; g_shot.done = std::move(done);
    ::SetTimer(g_hwnd, T_SHOT, 10000, nullptr);
    hr = g_wv->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, st.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([id](HRESULT r) -> HRESULT {
            shotFinished(id, r, false);
            return S_OK;
        }).Get());
    if (FAILED(hr)) shotFinished(id, hr, false);
}

// ---------------------------------------------------------------------------
// DevTools protocol plumbing
// ---------------------------------------------------------------------------
using CdpEventFn = std::function<void(const json::V&)>;
using CdpDoneFn  = std::function<void(HRESULT, const std::string&)>;

static void cdpSubscribe(const wchar_t* name, CdpEventFn fn) {
    ComPtr<ICoreWebView2DevToolsProtocolEventReceiver> r;
    if (FAILED(g_wv->GetDevToolsProtocolEventReceiver(name, &r)) || !r) {
        hostEvent(Ev("cdp-error").kv("subscribe", std::wstring(name)), true);
        return;
    }
    EventRegistrationToken tok{};
    r->add_DevToolsProtocolEventReceived(
        Callback<ICoreWebView2DevToolsProtocolEventReceivedEventHandler>(
            [fn](ICoreWebView2*, ICoreWebView2DevToolsProtocolEventReceivedEventArgs* a) -> HRESULT {
                LPWSTR js = nullptr;
                if (SUCCEEDED(a->get_ParameterObjectAsJson(&js)) && js) {
                    const std::string u = u8(js);
                    ::CoTaskMemFree(js);
                    json::V v;
                    if (json::parse(u, v)) fn(v);
                }
                return S_OK;
            }).Get(), &tok);
    g_receivers.push_back(r);
}

static void cdpCall(const std::wstring& method, const std::string& params, CdpDoneFn done) {
    if (!g_wv) { if (done) done(E_FAIL, std::string()); return; }
    const HRESULT hr = g_wv->CallDevToolsProtocolMethod(method.c_str(), wide(params).c_str(),
        Callback<ICoreWebView2CallDevToolsProtocolMethodCompletedHandler>(
            [done](HRESULT r, LPCWSTR res) -> HRESULT {
                if (done) done(r, res ? u8(res) : std::string());
                return S_OK;
            }).Get());
    if (FAILED(hr) && done) done(hr, std::string());
}

static std::string remoteObjText(const json::V& ro) {
    if (const json::V* v = ro.get("value")) return v->t == json::V::Str ? v->s : json::dump(*v);
    if (const json::V* u = ro.get("unserializableValue")) if (u->t == json::V::Str) return u->s;
    if (const json::V* d = ro.get("description")) if (d->t == json::V::Str) return d->s;
    return ro.str("type", "?");
}
static std::string header(const json::V* headers, const char* name) {   // case-insensitive
    if (!headers || headers->t != json::V::Obj) return std::string();
    for (const auto& kv : headers->o)
        if (::_stricmp(kv.first.c_str(), name) == 0)
            return kv.second.t == json::V::Str ? kv.second.s : json::dump(kv.second);
    return std::string();
}
static std::string shortUrl(const std::string& u) { return u.compare(0, 5, "data:") == 0 ? cap(u, 80) : cap(u, 1000); }

static void navigateStart() {
    g_navT0 = nowMs();
    const HRESULT hr = g_wv->Navigate(g_opt.url.c_str());
    if (FAILED(hr)) {
        hostEvent(Ev("nav").kb("ok", false).khr("hr", hr).kv("url", g_opt.url), true);
        beginExit(5, "Navigate() refused the URL");
    }
}

static void setupDevToolsThenNavigate() {
    cdpSubscribe(L"Runtime.consoleAPICalled", [](const json::V& p) {
        std::string text;
        if (const json::V* args = p.get("args"))
            if (args->t == json::V::Arr)
                for (size_t i = 0; i < args->a.size(); ++i) { if (i) text += ' '; text += remoteObjText(args->a[i]); }
        Ev ev("console");
        ev.kv("level", p.str("type")).kv("text", cap(text));
        if (const json::V* st = p.get("stackTrace"))
            if (const json::V* cf = st->get("callFrames"))
                if (cf->t == json::V::Arr && !cf->a.empty())
                    ev.kv("url", shortUrl(cf->a[0].str("url"))).kn("line", cf->a[0].num("lineNumber") + 1);
        hostEvent(ev);
    });
    cdpSubscribe(L"Runtime.exceptionThrown", [](const json::V& p) {
        const json::V* d = p.get("exceptionDetails");
        if (!d) return;
        Ev ev("exception");
        ev.kv("text", d->str("text"));
        if (const json::V* ex = d->get("exception")) ev.kv("desc", cap(ex->str("description")));
        ev.kv("url", shortUrl(d->str("url"))).kn("line", d->num("lineNumber") + 1);
        hostEvent(ev, true);
    });
    cdpSubscribe(L"Log.entryAdded", [](const json::V& p) {
        const json::V* e = p.get("entry");
        if (!e) return;
        Ev ev("log");
        ev.kv("source", e->str("source")).kv("level", e->str("level")).kv("text", cap(e->str("text")));
        if (e->get("url")) ev.kv("url", shortUrl(e->str("url")));
        hostEvent(ev);
    });
    if (g_opt.net) {
        cdpSubscribe(L"Network.requestWillBeSent", [](const json::V& p) {
            const json::V* r = p.get("request");
            if (!r) return;
            Ev ev("net");
            ev.kv("ev", "req").kv("id", p.str("requestId")).kv("type", p.str("type"))
              .kv("method", r->str("method")).kv("url", shortUrl(r->str("url")));
            const std::string range = header(r->get("headers"), "range");
            if (!range.empty()) ev.kv("range", range);
            hostEvent(ev);
        });
        cdpSubscribe(L"Network.responseReceived", [](const json::V& p) {
            const json::V* r = p.get("response");
            if (!r) return;
            const json::V* h = r->get("headers");
            Ev ev("net");
            ev.kv("ev", "resp").kv("id", p.str("requestId")).kv("type", p.str("type"))
              .kn("status", r->num("status")).kv("mime", r->str("mimeType")).kv("url", shortUrl(r->str("url")));
            const char* names[] = { "content-type", "content-range", "content-length", "accept-ranges",
                                    "access-control-allow-origin", "cache-control" };
            for (const char* n : names) { const std::string v = header(h, n); if (!v.empty()) ev.kv(n, v); }
            hostEvent(ev);
        });
        cdpSubscribe(L"Network.loadingFailed", [](const json::V& p) {
            Ev ev("net");
            ev.kv("ev", "fail").kv("id", p.str("requestId")).kv("type", p.str("type"))
              .kv("error", p.str("errorText"));
            if (const json::V* c = p.get("canceled")) if (c->t == json::V::Bool) ev.kb("canceled", c->b);
            if (p.get("blockedReason")) ev.kv("blocked", p.str("blockedReason"));
            if (const json::V* cs = p.get("corsErrorStatus")) ev.kv("cors", cs->str("corsError"));
            hostEvent(ev);
        });
        cdpSubscribe(L"Network.loadingFinished", [](const json::V& p) {
            hostEvent(Ev("net").kv("ev", "done").kv("id", p.str("requestId")).kn("bytes", p.num("encodedDataLength")));
        });
    }
    cdpCall(L"Runtime.enable", "{}", [](HRESULT hr, const std::string& r) {
        if (FAILED(hr)) hostEvent(Ev("cdp-error").kv("method", "Runtime.enable").khr("hr", hr).kraw("result", r), true);
        cdpCall(L"Log.enable", "{}", [](HRESULT hr2, const std::string& r2) {
            if (FAILED(hr2)) hostEvent(Ev("cdp-error").kv("method", "Log.enable").khr("hr", hr2).kraw("result", r2), true);
            if (!g_opt.net) { navigateStart(); return; }
            cdpCall(L"Network.enable", "{\"maxTotalBufferSize\":10000000,\"maxResourceBufferSize\":1000000}",
                [](HRESULT hr3, const std::string& r3) {
                    if (FAILED(hr3)) hostEvent(Ev("cdp-error").kv("method", "Network.enable").khr("hr", hr3).kraw("result", r3), true);
                    navigateStart();
                });
        });
    });
}

// ---------------------------------------------------------------------------
// --serve: answer requests for a host from a folder, uncached (product-like)
// ---------------------------------------------------------------------------
static const wchar_t* mimeFor(const std::wstring& path, bool& text) {
    static const struct { const wchar_t* ext; const wchar_t* mime; bool text; } table[] = {
        { L".html", L"text/html", true }, { L".htm", L"text/html", true }, { L".js", L"text/javascript", true },
        { L".mjs", L"text/javascript", true }, { L".css", L"text/css", true }, { L".json", L"application/json", true },
        { L".txt", L"text/plain", true }, { L".vtt", L"text/vtt", true }, { L".svg", L"image/svg+xml", true },
        { L".png", L"image/png", false }, { L".jpg", L"image/jpeg", false }, { L".jpeg", L"image/jpeg", false },
        { L".gif", L"image/gif", false }, { L".webp", L"image/webp", false }, { L".ico", L"image/x-icon", false },
        { L".woff", L"font/woff", false }, { L".woff2", L"font/woff2", false }, { L".ttf", L"font/ttf", false },
        { L".wasm", L"application/wasm", false }, { L".mp4", L"video/mp4", false }, { L".webm", L"video/webm", false },
        { L".mp3", L"audio/mpeg", false }, { L".wav", L"audio/wav", false }, { L".ogg", L"audio/ogg", false },
    };
    const size_t dot = path.find_last_of(L'.');
    const std::wstring ext = dot == std::wstring::npos ? std::wstring() : path.substr(dot);
    for (const auto& t : table) if (ieq(ext, t.ext)) { text = t.text; return t.mime; }
    text = false;
    return L"application/octet-stream";
}
static std::wstring pctDecode(const std::wstring& s) {
    const std::string in = u8(s);
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && std::isxdigit((unsigned char)in[i + 1]) && std::isxdigit((unsigned char)in[i + 2])) {
            out += (char)std::strtol(in.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else out += in[i];
    }
    return wide(out);
}
static void serveRequest(ICoreWebView2WebResourceRequestedEventArgs* a) {
    ComPtr<ICoreWebView2WebResourceRequest> req;
    if (FAILED(a->get_Request(&req)) || !req) return;
    LPWSTR raw = nullptr;
    if (FAILED(req->get_Uri(&raw)) || !raw) return;
    const std::wstring uri(raw);
    ::CoTaskMemFree(raw);
    for (const auto& s : g_opt.serves) {
        for (const wchar_t* scheme : { L"https://", L"http://" }) {
            const std::wstring prefix = std::wstring(scheme) + s.host + L"/";
            if (uri.size() < prefix.size() || ::_wcsnicmp(uri.c_str(), prefix.c_str(), prefix.size()) != 0) continue;
            std::wstring rel = uri.substr(prefix.size());
            const size_t q = rel.find_first_of(L"?#");
            if (q != std::wstring::npos) rel.resize(q);
            rel = pctDecode(rel);
            for (auto& c : rel) if (c == L'/') c = L'\\';
            int status = 200;
            std::wstring file;
            if (rel.find(L"..") != std::wstring::npos || rel.find(L':') != std::wstring::npos) status = 403;
            else {
                file = s.folder + L"\\" + rel;
                DWORD at = ::GetFileAttributesW(file.c_str());
                if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY)) {
                    if (file.back() != L'\\') file += L'\\';
                    file += L"index.html";
                    at = ::GetFileAttributesW(file.c_str());
                }
                if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) status = 404;
            }
            ComPtr<IStream> stream;
            std::wstring headers;
            if (status == 200 && FAILED(::SHCreateStreamOnFileEx(file.c_str(), STGM_READ | STGM_SHARE_DENY_NONE,
                                                                 FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, &stream)))
                status = 404;
            if (status == 200) {
                bool text = false;
                const wchar_t* mime = mimeFor(file, text);
                headers = std::wstring(L"Content-Type: ") + mime + (text ? L"; charset=utf-8" : L"")
                        + L"\r\nCache-Control: no-store, must-revalidate";
            } else {
                const std::string body = status == 403 ? std::string("403 forbidden") : "404 not found: " + u8(rel);
                stream.Attach(::SHCreateMemStream((const BYTE*)body.data(), (UINT)body.size()));
                headers = L"Content-Type: text/plain; charset=utf-8\r\nCache-Control: no-store";
            }
            ComPtr<ICoreWebView2WebResourceResponse> resp;
            const HRESULT hr = g_env->CreateWebResourceResponse(stream.Get(), status,
                status == 200 ? L"OK" : (status == 403 ? L"Forbidden" : L"Not Found"), headers.c_str(), &resp);
            if (SUCCEEDED(hr) && resp) a->put_Response(resp.Get());
            if (g_opt.logServe || status != 200) hostEvent(Ev("serve").kv("url", uri).kn("status", status), status != 200);
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Page messages
// ---------------------------------------------------------------------------
static void onPageMessage(const std::string& raw) {
    const std::string msg = raw.empty() ? std::string("null") : raw;   // keep every --out line valid JSON
    g_msgs.push_back(msg);
    emitLine(msg, !g_opt.quiet);
    if (g_exiting) return;
    if (g_waiting && msg.find(g_waitSub) != std::string::npos) {
        g_waiting = false;
        ::KillTimer(g_hwnd, T_WAITFOR);
        hostEvent(Ev("waitfor").kn("line", g_waitLine).kv("sub", g_waitSub).kn("ms", nowMs() - g_waitT0));
        feedNext();
    }
    for (const auto& s : g_opt.exitOn)
        if (msg.find(s) != std::string::npos) { beginExit(0, "exit-on: " + s); return; }
}

// ---------------------------------------------------------------------------
// The feed
// ---------------------------------------------------------------------------
static const char* const kCommands[] = { "__sleep", "__eval", "__gesture", "__cdp", "__shot", "__waitfor",
                                         "__exit", "__nav", "__size", "__mark", "__setfiles",
                                         "__map", "__unmap" };   // WC review: mappings set AFTER the page loaded
static std::wstring absPath(const std::wstring& p);   // defined with the command line, below
static std::wstring stripSlash(std::wstring p);

static std::string feedCommand(const json::V& v) {
    if (v.t == json::V::Obj && !v.o.empty() && v.o[0].first.compare(0, 2, "__") == 0) return v.o[0].first;
    return std::string();
}

static bool loadFeed(std::string& err) {
    HANDLE h = ::CreateFileW(g_opt.feed.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot open feed file"; return false; }
    LARGE_INTEGER sz{};
    ::GetFileSizeEx(h, &sz);
    std::string body((size_t)sz.QuadPart, '\0');
    DWORD rd = 0;
    if (!body.empty()) ::ReadFile(h, &body[0], (DWORD)body.size(), &rd, nullptr);
    ::CloseHandle(h);
    body.resize(rd);
    if (body.size() >= 3 && (unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body.erase(0, 3);
    int no = 0;
    size_t at = 0;
    while (at <= body.size()) {
        const size_t nl = body.find('\n', at);
        std::string line = body.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        ++no;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t lead = 0;
        while (lead < line.size() && (line[lead] == ' ' || line[lead] == '\t')) ++lead;
        line.erase(0, lead);
        if (!line.empty() && line[0] != '#' && line.compare(0, 2, "//") != 0) {
            json::V v;
            if (!json::parse(line, v)) { err = "feed line " + std::to_string(no) + " is not valid JSON"; return false; }
            const std::string cmd = feedCommand(v);
            if (!cmd.empty()) {
                bool known = false;
                for (const char* k : kCommands) if (cmd == k) known = true;
                if (!known) { err = "feed line " + std::to_string(no) + ": unknown command " + cmd; return false; }
                const json::V& arg = v.o[0].second;
                const bool wantNum = cmd == "__sleep" || cmd == "__exit";
                const bool wantStr = !wantNum && cmd != "__mark";
                if ((wantNum && arg.t != json::V::Num) || (wantStr && arg.t != json::V::Str)) {
                    err = "feed line " + std::to_string(no) + ": " + cmd + " has the wrong argument type"; return false;
                }
            }
            g_feed.push_back({ no, line });
        }
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return true;
}

static void finishFeed() {
    if (g_feedDone) return;
    g_feedDone = true;
    hostEvent(Ev("feed-done").kn("lines", (double)g_feed.size()));
    if (g_opt.exitOn.empty()) beginExit(0, "feed-done");
}

static void startFeed() {
    g_feedStarted = true;
    if (g_opt.feed.empty()) return;                      // no feed: wait for --exit-on / --timeout
    ::PostMessageW(g_hwnd, WM_APP_FEED, 0, 0);
}

static void feedNext() {
    g_feedBusy = false;
    if (!g_exiting) ::PostMessageW(g_hwnd, WM_APP_FEED, 0, 0);
}

static void resizeView(int w, int h) {
    if (w < 100 || h < 100) return;
    RECT wr{};
    ::GetWindowRect(g_hwnd, &wr);
    // keep the top-left corner (or the bottom-right one in the primary-corner fallback)
    int x = wr.left, y = wr.top;
    if (g_anchorBottomRight) { x = wr.right - w; y = wr.bottom - h; }
    ::SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    if (g_ctl) { RECT rc; ::GetClientRect(g_hwnd, &rc); g_ctl->put_Bounds(rc); }
}

static void feedPump() {
    if (g_exiting || g_feedBusy || g_feedDone || !g_feedStarted || g_opt.feed.empty()) return;
    while (g_feedPos < g_feed.size()) {
        const FeedLine fl = g_feed[g_feedPos++];
        const size_t mark = g_prevMark;
        g_prevMark = g_msgs.size();
        json::V v;
        json::parse(fl.text, v);                         // validated at load
        const std::string cmd = feedCommand(v);
        const int line = fl.no;

        if (cmd.empty()) {                               // a message for the page
            const HRESULT hr = g_wv->PostWebMessageAsJson(wide(fl.text).c_str());
            Ev ev("post");
            ev.kn("line", line).kraw("msg", fl.text);
            if (FAILED(hr)) ev.khr("hr", hr);
            hostEvent(ev, FAILED(hr));
            continue;
        }
        const json::V& arg = v.o[0].second;
        if (cmd == "__mark") { hostEvent(Ev("mark").kn("line", line).kraw("value", json::dump(arg))); continue; }
        if (cmd == "__size") {
            int w = 0, h = 0;
            if (std::sscanf(arg.s.c_str(), "%dx%d", &w, &h) == 2) resizeView(w, h);
            hostEvent(Ev("size").kn("line", line).kn("w", w).kn("h", h));
            continue;
        }
        if (cmd == "__map" || cmd == "__unmap") {
            // {"__map":"<host>=<folder>[:allow|deny|denycors]"} / {"__unmap":"<host>"}: the SAME call the
            // product makes, at the same moment it makes it -- after the page has loaded (AudioPBConfig
            // maps a file's folder when the file is opened, long after translator.html navigated).
            ComPtr<ICoreWebView2_3> w3;
            g_wv.As(&w3);
            const std::wstring v = wide(arg.s);
            std::wstring host = v, folder;
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS;
            std::string kindName = "denycors";
            HRESULT hr = E_INVALIDARG;
            if (cmd == "__map") {
                const size_t eq = v.find(L'=');
                if (eq != std::wstring::npos && eq > 0 && eq + 1 < v.size()) {
                    host = v.substr(0, eq);
                    folder = v.substr(eq + 1);
                    const size_t c = folder.find_last_of(L':');
                    if (c != std::wstring::npos && c > 1) {          // c == 1 is the drive colon
                        const std::wstring k = folder.substr(c + 1);
                        if (ieq(k, L"allow"))         { kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW; kindName = "allow"; folder.resize(c); }
                        else if (ieq(k, L"deny"))     { kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY; kindName = "deny"; folder.resize(c); }
                        else if (ieq(k, L"denycors")) { kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS; kindName = "denycors"; folder.resize(c); }
                    }
                    folder = stripSlash(absPath(folder));
                    hr = w3 ? w3->SetVirtualHostNameToFolderMapping(host.c_str(), folder.c_str(), kind) : E_NOINTERFACE;
                }
                hostEvent(Ev("map").kn("line", line).kb("runtime", true).kv("host", host).kv("folder", folder)
                          .kv("kind", kindName).khr("hr", hr), FAILED(hr));
            } else {
                hr = w3 ? w3->ClearVirtualHostNameToFolderMapping(host.c_str()) : E_NOINTERFACE;
                hostEvent(Ev("unmap").kn("line", line).kv("host", host).khr("hr", hr), FAILED(hr));
            }
            continue;
        }
        if (cmd == "__exit") { beginExit((int)arg.n, "feed __exit (line " + std::to_string(line) + ")"); return; }
        if (cmd == "__sleep") {
            g_feedBusy = true;
            ::SetTimer(g_hwnd, T_SLEEP, (UINT)(arg.n < 1 ? 1 : arg.n), nullptr);
            return;
        }
        if (cmd == "__waitfor") {
            bool already = false;
            for (size_t i = mark; i < g_msgs.size() && !already; ++i)
                if (g_msgs[i].find(arg.s) != std::string::npos) already = true;
            if (already) { hostEvent(Ev("waitfor").kn("line", line).kv("sub", arg.s).kb("already", true)); continue; }
            g_waiting = true; g_waitSub = arg.s; g_waitLine = line; g_waitT0 = nowMs();
            g_feedBusy = true;
            const double ms = v.num("ms", 10000);
            ::SetTimer(g_hwnd, T_WAITFOR, (UINT)(ms < 1 ? 1 : ms), nullptr);
            return;
        }
        if (cmd == "__eval") {
            g_feedBusy = true;
            const double t0 = nowMs();
            const HRESULT hr = g_wv->ExecuteScript(wide(arg.s).c_str(),
                Callback<ICoreWebView2ExecuteScriptCompletedHandler>([line, t0](HRESULT r, LPCWSTR res) -> HRESULT {
                    Ev ev("eval");
                    ev.kn("line", line).kn("ms", nowMs() - t0);
                    if (FAILED(r)) ev.khr("hr", r);
                    ev.kraw("result", res ? u8(res) : std::string("null"));
                    hostEvent(ev, FAILED(r));
                    feedNext();
                    return S_OK;
                }).Get());
            if (FAILED(hr)) { hostEvent(Ev("eval").kn("line", line).khr("hr", hr), true); feedNext(); }
            return;
        }
        if (cmd == "__gesture" || cmd == "__cdp") {
            g_feedBusy = true;
            std::wstring method = L"Runtime.evaluate";
            std::string params;
            if (cmd == "__gesture") {
                params = "{\"expression\":" + jq(arg.s) + ",\"userGesture\":true,\"awaitPromise\":true,\"returnByValue\":true}";
            } else {
                method = wide(arg.s);
                const json::V* p = v.get("params");
                params = p ? json::dump(*p) : std::string("{}");
            }
            const std::string kind = cmd.substr(2);
            const double t0 = nowMs();
            cdpCall(method, params, [line, kind, t0](HRESULT r, const std::string& res) {
                Ev ev(kind.c_str());
                ev.kn("line", line).kn("ms", nowMs() - t0);
                if (FAILED(r)) ev.khr("hr", r);
                ev.kraw("result", res.empty() ? std::string("null") : res);
                hostEvent(ev, FAILED(r));
                feedNext();
            });
            return;
        }
        if (cmd == "__shot") {
            g_feedBusy = true;
            takeShot(wide(arg.s), line, [](bool) { feedNext(); });
            return;
        }
        if (cmd == "__nav") {
            g_feedBusy = true;
            g_navWaitFeed = true;
            g_navT0 = nowMs();
            const HRESULT hr = g_wv->Navigate(wide(arg.s).c_str());
            if (FAILED(hr)) {
                g_navWaitFeed = false;
                hostEvent(Ev("nav").kn("line", line).kb("ok", false).khr("hr", hr), true);
                feedNext();
            }
            return;
        }
        if (cmd == "__setfiles") {
            g_feedBusy = true;
            const std::string sel = arg.s;
            const json::V* files = v.get("files");
            const std::string filesJson = files ? json::dump(*files) : std::string("[]");
            cdpCall(L"DOM.enable", "{}", [sel, filesJson, line](HRESULT, const std::string&) {
                const std::string expr = "document.querySelector(" + jq(sel) + ")";
                cdpCall(L"Runtime.evaluate", "{\"expression\":" + jq(expr) + "}",
                    [filesJson, line](HRESULT hr, const std::string& res) {
                        json::V rv;
                        std::string objId;
                        if (SUCCEEDED(hr) && json::parse(res, rv))
                            if (const json::V* r = rv.get("result")) objId = r->str("objectId");
                        if (objId.empty()) {
                            hostEvent(Ev("setfiles").kn("line", line).kb("ok", false).kv("err", "selector matched nothing")
                                      .kraw("result", res.empty() ? std::string("null") : res), true);
                            feedNext();
                            return;
                        }
                        cdpCall(L"DOM.setFileInputFiles", "{\"files\":" + filesJson + ",\"objectId\":" + jq(objId) + "}",
                            [line](HRESULT h2, const std::string& r2) {
                                hostEvent(Ev("setfiles").kn("line", line).kb("ok", SUCCEEDED(h2))
                                          .kraw("result", r2.empty() ? std::string("null") : r2), FAILED(h2));
                                feedNext();
                            });
                    });
            });
            return;
        }
    }
    finishFeed();
}

// ---------------------------------------------------------------------------
// Exit (always through here; never hangs: T_EXITGRACE + the watchdog)
// ---------------------------------------------------------------------------
static void beginExit(int code, const std::string& reason) {
    if (g_exiting) return;
    g_exiting = true;
    g_exitCode = code;
    ::KillTimer(g_hwnd, T_TIMEOUT);
    ::KillTimer(g_hwnd, T_SLEEP);
    ::KillTimer(g_hwnd, T_WAITFOR);
    hostEvent(Ev("exit-begin").kn("code", code).kv("reason", reason), true);
    checkFocus("exit");
    ::SetTimer(g_hwnd, T_EXITGRACE, 12000, nullptr);
    if (!g_opt.shot.empty() && g_wv) takeShot(g_opt.shot, 0, [](bool) { finishExit(); });
    else finishExit();
}

static void finishExit() {
    if (g_exitFinished) return;
    g_exitFinished = true;
    ::KillTimer(g_hwnd, T_EXITGRACE);
    hostEvent(Ev("exit").kn("code", g_exitCode), true);
    if (g_ctl) g_ctl->Close();
    if (g_hwnd) ::DestroyWindow(g_hwnd);
    else ::PostQuitMessage(g_exitCode);
}

static DWORD WINAPI watchdogProc(LPVOID) {
    ::Sleep(g_opt.timeoutMs + 20000);
    const std::string l = "{\"__host\":\"watchdog\",\"code\":3}\n";
    if (::TryEnterCriticalSection(&g_outLock)) {
        writeAll(g_outFile, l);
        writeAll(g_stdout, l);
        ::LeaveCriticalSection(&g_outLock);
    }
    ::TerminateProcess(::GetCurrentProcess(), 3);
    return 0;
}

// ---------------------------------------------------------------------------
// WebView2 creation
// ---------------------------------------------------------------------------
static void onController(HRESULT r, ICoreWebView2Controller* c) {
    if (FAILED(r) || !c) {
        hostEvent(Ev("controller-error").khr("hr", r), true);
        beginExit(1, "controller creation failed");
        return;
    }
    if (g_exiting) return;
    g_ctl = c;
    g_ctl->get_CoreWebView2(&g_wv);
    if (!g_wv) { beginExit(1, "no CoreWebView2"); return; }

    RECT rc;
    ::GetClientRect(g_hwnd, &rc);
    g_ctl->put_Bounds(rc);
    double scaleApplied = 0;
    if (g_opt.scale > 0) {
        ComPtr<ICoreWebView2Controller3> c3;
        if (SUCCEEDED(g_ctl.As(&c3)) && c3) {
            c3->put_ShouldDetectMonitorScaleChanges(FALSE);
            c3->put_RasterizationScale(g_opt.scale);
            c3->get_RasterizationScale(&scaleApplied);
        }
    } else {
        ComPtr<ICoreWebView2Controller3> c3;
        if (SUCCEEDED(g_ctl.As(&c3)) && c3) c3->get_RasterizationScale(&scaleApplied);
    }
    g_ctl->put_IsVisible(TRUE);

    ComPtr<ICoreWebView2Settings> st;
    if (SUCCEEDED(g_wv->get_Settings(&st)) && st && g_opt.productSettings) {
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        st->put_AreDevToolsEnabled(FALSE);
    }
    HRESULT ssHr = S_FALSE;
    if (st && g_opt.noSmartScreen) {
        ComPtr<ICoreWebView2Settings8> s8;
        ssHr = SUCCEEDED(st.As(&s8)) && s8 ? s8->put_IsReputationCheckingRequired(FALSE) : E_NOINTERFACE;
    }
    bool muted = false;
    if (!g_opt.sound) {
        ComPtr<ICoreWebView2_8> w8;
        if (SUCCEEDED(g_wv.As(&w8)) && w8 && SUCCEEDED(w8->put_IsMuted(TRUE))) muted = true;
    }
    hostEvent(Ev("view").kn("w", rc.right - rc.left).kn("h", rc.bottom - rc.top).kn("rasterScale", scaleApplied)
              .kn("dpi", ::GetDpiForWindow(g_hwnd)).kb("muted", muted).kb("productSettings", g_opt.productSettings)
              .kv("smartScreen", g_opt.noSmartScreen ? (SUCCEEDED(ssHr) ? "off" : "off-FAILED " + hrs(ssHr)) : std::string("default")));

    // Folder mappings (served INTERNALLY by WebView2: WebResourceRequested never sees them)
    ComPtr<ICoreWebView2_3> w3;
    g_wv.As(&w3);
    for (const auto& m : g_opt.maps) {
        const HRESULT hr = w3 ? w3->SetVirtualHostNameToFolderMapping(m.host.c_str(), m.folder.c_str(), m.kind) : E_NOINTERFACE;
        hostEvent(Ev("map").kv("host", m.host).kv("folder", m.folder).kv("kind", m.kindName).khr("hr", hr), FAILED(hr));
        if (FAILED(hr)) { beginExit(1, "SetVirtualHostNameToFolderMapping failed"); return; }
    }
    // Served hosts (WebResourceRequested, Cache-Control: no-store -- like patchbus.local)
    EventRegistrationToken tok{};
    for (const auto& s : g_opt.serves) {
        g_wv->AddWebResourceRequestedFilter((L"https://" + s.host + L"/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        g_wv->AddWebResourceRequestedFilter((L"http://" + s.host + L"/*").c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        hostEvent(Ev("serve-host").kv("host", s.host).kv("folder", s.folder));
    }
    if (!g_opt.serves.empty())
        g_wv->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT { serveRequest(a); return S_OK; }).Get(), &tok);

    g_wv->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
            LPWSTR raw = nullptr;
            std::string msg;
            if (SUCCEEDED(a->get_WebMessageAsJson(&raw)) && raw) { msg = u8(raw); ::CoTaskMemFree(raw); }
            // postMessageWithAdditionalObjects: report the File paths the page handed over
            ComPtr<ICoreWebView2WebMessageReceivedEventArgs2> a2;
            if (SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&a2))) && a2) {
                ComPtr<ICoreWebView2ObjectCollectionView> objs;
                UINT32 n = 0;
                if (SUCCEEDED(a2->get_AdditionalObjects(&objs)) && objs && SUCCEEDED(objs->get_Count(&n)) && n) {
                    std::string arr = "[";
                    for (UINT32 i = 0; i < n; ++i) {
                        if (i) arr += ',';
                        ComPtr<IUnknown> o;
                        ComPtr<ICoreWebView2File> f;
                        LPWSTR p = nullptr;
                        if (SUCCEEDED(objs->GetValueAtIndex(i, &o)) && o && SUCCEEDED(o.As(&f)) && f && SUCCEEDED(f->get_Path(&p)) && p) {
                            arr += jq(u8(p));
                            ::CoTaskMemFree(p);
                        } else arr += "null";
                    }
                    arr += "]";
                    hostEvent(Ev("msg-objects").kn("count", n).kraw("files", arr), true);
                }
            }
            onPageMessage(msg);
            return S_OK;
        }).Get(), &tok);

    g_wv->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
        [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
            LPWSTR uri = nullptr;
            a->get_Uri(&uri);
            hostEvent(Ev("nav-start").kv("url", shortUrl(u8(uri))));
            if (uri) ::CoTaskMemFree(uri);
            return S_OK;
        }).Get(), &tok);

    g_wv->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
        [](ICoreWebView2* s, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
            BOOL ok = FALSE;
            a->get_IsSuccess(&ok);
            COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
            a->get_WebErrorStatus(&st);
            int http = 0;
            ComPtr<ICoreWebView2NavigationCompletedEventArgs2> a2;
            if (SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&a2))) && a2) a2->get_HttpStatusCode(&http);
            LPWSTR src = nullptr;
            s->get_Source(&src);
            hostEvent(Ev("nav").kb("ok", ok != FALSE).kn("webError", (double)st).kn("http", http)
                      .kv("url", shortUrl(u8(src))).kn("ms", nowMs() - g_navT0), true);
            if (src) ::CoTaskMemFree(src);
            checkFocus("nav");
            if (!g_firstNavDone) {
                g_firstNavDone = true;
                if (!ok) { beginExit(5, "first navigation failed"); return S_OK; }
                startFeed();
            } else if (g_navWaitFeed) {
                g_navWaitFeed = false;
                feedNext();
            }
            return S_OK;
        }).Get(), &tok);

    g_wv->add_ProcessFailed(Callback<ICoreWebView2ProcessFailedEventHandler>(
        [](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
            COREWEBVIEW2_PROCESS_FAILED_KIND k = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
            a->get_ProcessFailedKind(&k);
            hostEvent(Ev("process-failed").kn("kind", (double)k), true);
            if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) beginExit(6, "browser process exited");
            else if (k == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) beginExit(6, "render process exited");
            return S_OK;
        }).Get(), &tok);

    g_wv->add_ContainsFullScreenElementChanged(Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
        [](ICoreWebView2* s, IUnknown*) -> HRESULT {
            BOOL fs = FALSE;
            s->get_ContainsFullScreenElement(&fs);
            hostEvent(Ev("fullscreen").kb("on", fs != FALSE));
            return S_OK;
        }).Get(), &tok);

    setupDevToolsThenNavigate();
}

static void createEnv() {
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!g_opt.browserArgs.empty()) opts->put_AdditionalBrowserArguments(g_opt.browserArgs.c_str());
    const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(nullptr, g_opt.udf.c_str(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (g_exiting) return S_OK;
                if (FAILED(r) || !env) {
                    // e.g. the previous run's browser process still holds this user-data folder
                    hostEvent(Ev("env-error").khr("hr", r).kn("attempt", g_envAttempts + 1), true);
                    if (++g_envAttempts < 10) ::SetTimer(g_hwnd, T_ENVRETRY, 500, nullptr);
                    else beginExit(1, "environment creation failed");
                    return S_OK;
                }
                g_env = env;
                env->CreateCoreWebView2Controller(g_hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT r2, ICoreWebView2Controller* c) -> HRESULT { onController(r2, c); return S_OK; }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        hostEvent(Ev("env-error").khr("hr", hr).kb("sync", true).kn("attempt", g_envAttempts + 1), true);
        if (++g_envAttempts < 10) ::SetTimer(g_hwnd, T_ENVRETRY, 500, nullptr);
        else beginExit(1, "environment creation failed");
    }
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_APP_FEED:
        feedPump();
        return 0;
    case WM_TIMER:
        switch (wp) {
        case T_TIMEOUT:
            ::KillTimer(h, T_TIMEOUT);
            hostEvent(Ev("timeout").kn("ms", g_opt.timeoutMs), true);
            beginExit(2, "timeout");
            return 0;
        case T_SLEEP:
            ::KillTimer(h, T_SLEEP);
            feedNext();
            return 0;
        case T_WAITFOR:
            ::KillTimer(h, T_WAITFOR);
            if (g_waiting) {
                g_waiting = false;
                hostEvent(Ev("waitfor-timeout").kn("line", g_waitLine).kv("sub", g_waitSub), true);
                beginExit(4, "waitfor timeout (line " + std::to_string(g_waitLine) + ")");
            }
            return 0;
        case T_SHOT:
            ::KillTimer(h, T_SHOT);
            shotFinished(g_shot.id, E_ABORT, true);
            return 0;
        case T_EXITGRACE:
            ::KillTimer(h, T_EXITGRACE);
            hostEvent(Ev("exit-grace-expired"), true);
            finishExit();
            return 0;
        case T_ENVRETRY:
            ::KillTimer(h, T_ENVRETRY);
            if (!g_exiting) createEnv();
            return 0;
        }
        break;
    case WM_SIZE:
        if (g_ctl) { RECT rc; ::GetClientRect(h, &rc); g_ctl->put_Bounds(rc); }
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_CLOSE:
        beginExit(0, "window closed");
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(g_exitCode);
        return 0;
    }
    return ::DefWindowProcW(h, m, wp, lp);
}

// ---------------------------------------------------------------------------
// Command line
// ---------------------------------------------------------------------------
static void usage() {
    const char* u =
        "wv2probe -- WebView2 test instrument (AudioPB)\n"
        "usage: wv2probe --url <url> [options]\n"
        "  --url <url>                     page to load (required)\n"
        "  --map <host>=<folder>[:allow|deny|denycors]   SetVirtualHostNameToFolderMapping (repeatable; default denycors)\n"
        "  --serve <host>=<folder>         serve https://<host>/ from <folder> via WebResourceRequested,\n"
        "                                  Cache-Control: no-store, like the product's patchbus.local (repeatable)\n"
        "  --feed <file.jsonl>             after the first NavigationCompleted, run each line: plain JSON is\n"
        "                                  PostWebMessageAsJson'd; {\"__sleep\":ms} {\"__eval\":\"js\"} {\"__shot\":\"x.png\"}\n"
        "                                  {\"__waitfor\":\"substr\",\"ms\":10000} {\"__gesture\":\"js\"} {\"__cdp\":\"Method\",\"params\":{}}\n"
        "                                  {\"__nav\":\"url\"} {\"__size\":\"WxH\"} {\"__mark\":any} {\"__setfiles\":\"css\",\"files\":[..]}\n"
        "                                  {\"__map\":\"host=folder[:kind]\"} {\"__unmap\":\"host\"}  (mapping AFTER load, like the product)\n"
        "                                  {\"__exit\":code}. '#' or '//' lines are comments. Feed end = exit 0\n"
        "                                  unless --exit-on is given (then keep waiting for it).\n"
        "  --out <file.jsonl>              append every page message (raw JSON) + host events ({\"__host\":...})\n"
        "  --exit-on <substring>           exit 0 when a page message contains it (repeatable)\n"
        "  --timeout <ms>                  hard end of the run, exit code 2 (default 60000)\n"
        "  --shot <file.png>               CapturePreview PNG at exit\n"
        "  --size <W>x<H>                  client size in physical px (default 1280x800)\n"
        "  --pos <X>,<Y>                   window top-left in virtual-screen px (default -1880,40 = inside the\n"
        "                                  second screen left of the primary). If no monitor holds that point/rect\n"
        "                                  the window goes to the bottom-right of the primary. Never activated.\n"
        "  --scale <f>|auto                RasterizationScale (default 1 = CSS px == screenshot px)\n"
        "  --udf <folder>                  WebView2 user-data folder (default <exe dir>\\udf)\n"
        "  --browser-args \"<args>\"         AdditionalBrowserArguments\n"
        "  --net                           log CDP Network events (requests, ranges, statuses, failures)\n"
        "  --log-serve                     log every request answered by --serve\n"
        "  --sound                         do NOT mute the WebView (muted by default)\n"
        "  --product-settings              context menus/status bar/devtools off, like AudioPBConfig\n"
        "  --no-smartscreen                ICoreWebView2Settings8::put_IsReputationCheckingRequired(FALSE)\n"
        "  --quiet                         only important lines on stdout (the --out file gets everything)\n"
        "exit codes: 0 ok | 1 bad args/init | 2 timeout | 3 watchdog | 4 waitfor timeout | 5 navigation failed\n"
        "            | 6 browser/render process died | other = {\"__exit\":code}\n";
    writeAll(::GetStdHandle(STD_ERROR_HANDLE), u);
}

static std::wstring absPath(const std::wstring& p) {
    wchar_t full[2048] = {};
    if (::GetFullPathNameW(p.c_str(), 2048, full, nullptr)) return full;
    return p;
}
static std::wstring stripSlash(std::wstring p) {
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

static bool parseArgs(int argc, wchar_t** argv, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&](std::wstring& out) -> bool {
            if (i + 1 >= argc) { err = "missing value after " + u8(a); return false; }
            out = argv[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--help" || a == L"-h" || a == L"/?") { err = "help"; return false; }
        else if (a == L"--url")  { if (!next(v)) return false; g_opt.url = v; }
        else if (a == L"--feed") { if (!next(v)) return false; g_opt.feed = absPath(v); }
        else if (a == L"--out")  { if (!next(v)) return false; g_opt.out = absPath(v); }
        else if (a == L"--shot") { if (!next(v)) return false; g_opt.shot = absPath(v); }
        else if (a == L"--udf")  { if (!next(v)) return false; g_opt.udf = stripSlash(absPath(v)); }
        else if (a == L"--browser-args") { if (!next(v)) return false; g_opt.browserArgs = v; }
        else if (a == L"--pos") {
            if (!next(v)) return false;
            if (std::swscanf(v.c_str(), L"%d,%d", &g_opt.posX, &g_opt.posY) != 2) { err = "bad --pos (X,Y)"; return false; }
        }
        else if (a == L"--exit-on") { if (!next(v)) return false; g_opt.exitOn.push_back(u8(v)); }
        else if (a == L"--timeout") {
            if (!next(v)) return false;
            const long t = std::wcstol(v.c_str(), nullptr, 10);
            if (t < 1000 || t > 86400000) { err = "--timeout must be 1000..86400000 ms"; return false; }
            g_opt.timeoutMs = (DWORD)t;
        }
        else if (a == L"--size") {
            if (!next(v)) return false;
            if (std::swscanf(v.c_str(), L"%dx%d", &g_opt.w, &g_opt.h) != 2 || g_opt.w < 100 || g_opt.h < 100) { err = "bad --size (WxH)"; return false; }
        }
        else if (a == L"--scale") {
            if (!next(v)) return false;
            if (ieq(v, L"auto")) g_opt.scale = 0;
            else { g_opt.scale = std::wcstod(v.c_str(), nullptr); if (g_opt.scale <= 0 || g_opt.scale > 4) { err = "bad --scale"; return false; } }
        }
        else if (a == L"--map" || a == L"--serve") {
            if (!next(v)) return false;
            const size_t eq = v.find(L'=');
            if (eq == std::wstring::npos || eq == 0 || eq + 1 >= v.size()) { err = "expected <host>=<folder> after " + u8(a); return false; }
            std::wstring host = v.substr(0, eq), folder = v.substr(eq + 1);
            COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS;
            std::string kindName = "denycors";
            if (a == L"--map") {
                const size_t c = folder.find_last_of(L':');
                if (c != std::wstring::npos && c > 1) {          // c == 1 is the drive colon
                    const std::wstring k = folder.substr(c + 1);
                    if (ieq(k, L"allow"))         { kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW; kindName = "allow"; folder.resize(c); }
                    else if (ieq(k, L"deny"))     { kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY; kindName = "deny"; folder.resize(c); }
                    else if (ieq(k, L"denycors")) { kind = COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS; kindName = "denycors"; folder.resize(c); }
                    else { err = "unknown access kind '" + u8(k) + "' (allow|deny|denycors)"; return false; }
                }
            }
            folder = stripSlash(absPath(folder));
            const DWORD at = ::GetFileAttributesW(folder.c_str());
            if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) { err = "not a folder: " + u8(folder); return false; }
            if (a == L"--map") g_opt.maps.push_back({ host, folder, kind, kindName });
            else g_opt.serves.push_back({ host, folder });
        }
        else if (a == L"--net") g_opt.net = true;
        else if (a == L"--log-serve") g_opt.logServe = true;
        else if (a == L"--sound") g_opt.sound = true;
        else if (a == L"--product-settings") g_opt.productSettings = true;
        else if (a == L"--no-smartscreen") g_opt.noSmartScreen = true;
        else if (a == L"--quiet") g_opt.quiet = true;
        else { err = "unknown argument: " + u8(a); return false; }
    }
    if (g_opt.url.empty()) { err = "--url is required"; return false; }
    return true;
}

// Monitors, for the start event (diagnoses a fallback placement at a glance).
static BOOL CALLBACK monitorEnum(HMONITOR m, HDC, LPRECT, LPARAM lp) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(m, &mi)) {
        std::string& s = *reinterpret_cast<std::string*>(lp);
        if (s.size() > 1) s += ',';
        s += "{\"name\":" + jq(u8(mi.szDevice)) + ",\"x\":" + jn(mi.rcMonitor.left) + ",\"y\":" + jn(mi.rcMonitor.top)
           + ",\"w\":" + jn(mi.rcMonitor.right - mi.rcMonitor.left) + ",\"h\":" + jn(mi.rcMonitor.bottom - mi.rcMonitor.top)
           + ",\"primary\":" + ((mi.dwFlags & MONITORINFOF_PRIMARY) ? "true" : "false") + "}";
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int wmain(int argc, wchar_t** argv) {
    ::QueryPerformanceFrequency(&g_qpf);
    ::QueryPerformanceCounter(&g_qp0);
    ::InitializeCriticalSection(&g_outLock);
    g_stdout = ::GetStdHandle(STD_OUTPUT_HANDLE);

    std::string err;
    if (!parseArgs(argc, argv, err)) {
        if (err != "help") writeAll(::GetStdHandle(STD_ERROR_HANDLE), "wv2probe: " + err + "\n");
        usage();
        return err == "help" ? 0 : 1;
    }
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1;

    if (!g_opt.out.empty()) {
        std::wstring dir = g_opt.out;
        const size_t sl = dir.find_last_of(L"\\/");
        if (sl != std::wstring::npos) { dir.resize(sl); ::SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); }
        g_outFile = ::CreateFileW(g_opt.out.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_outFile == INVALID_HANDLE_VALUE) { writeAll(::GetStdHandle(STD_ERROR_HANDLE), "wv2probe: cannot open --out\n"); return 1; }
    }

    LPWSTR ver = nullptr;
    const HRESULT hv = ::GetAvailableCoreWebView2BrowserVersionString(nullptr, &ver);
    const std::string runtime = ver ? u8(ver) : std::string();
    if (ver) ::CoTaskMemFree(ver);
    writeAll(g_stdout, std::string("wv2probe ") + kToolVersion + " | WebView2 runtime " + (runtime.empty() ? "NOT FOUND" : runtime)
                       + " | SDK " + kSdkVersion + "\n");
    if (FAILED(hv) || runtime.empty()) { emitLine("{\"__host\":\"start\",\"error\":\"no WebView2 runtime\"}", true); return 1; }

    if (!g_opt.feed.empty() && !loadFeed(err)) {
        writeAll(::GetStdHandle(STD_ERROR_HANDLE), "wv2probe: " + err + "\n");
        emitLine(Ev("feed-error").kv("error", err).str(), true);
        return 1;
    }
    if (g_opt.udf.empty()) {
        wchar_t exe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        ::PathRemoveFileSpecW(exe);
        g_opt.udf = std::wstring(exe) + L"\\udf";
    }
    ::SHCreateDirectoryExW(nullptr, g_opt.udf.c_str(), nullptr);

    ::CreateThread(nullptr, 0, watchdogProc, nullptr, 0, nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = ::GetModuleHandleW(nullptr);
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)::GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"wv2probeWindow";
    ::RegisterClassExW(&wc);

    // Placement: --pos (default = the second screen). Only if no monitor holds that point AND
    // rect (screen unplugged, layout changed) -> bottom-right of the primary work area.
    int x = g_opt.posX, y = g_opt.posY;
    std::string placement = "pos";
    {
        RECT want{ x, y, x + g_opt.w, y + g_opt.h };
        const POINT tl{ x, y };
        if (!::MonitorFromRect(&want, MONITOR_DEFAULTTONULL) || !::MonitorFromPoint(tl, MONITOR_DEFAULTTONULL)) {
            RECT wa{};
            ::SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            x = wa.right - g_opt.w;
            y = wa.bottom - g_opt.h;
            placement = "fallback: no monitor at --pos, bottom-right of primary";
            g_anchorBottomRight = true;
        }
    }
    // WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE: never takes the foreground; TOOLWINDOW: no taskbar button.
    g_hwnd = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, wc.lpszClassName, L"wv2probe",
                               WS_POPUP | WS_CLIPCHILDREN, x, y, g_opt.w, g_opt.h,
                               nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) { emitLine("{\"__host\":\"start\",\"error\":\"CreateWindowEx failed\"}", true); return 1; }
    ::ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

    RECT wr{};
    ::GetWindowRect(g_hwnd, &wr);
    std::string monitors = "[";
    ::EnumDisplayMonitors(nullptr, nullptr, monitorEnum, reinterpret_cast<LPARAM>(&monitors));
    monitors += "]";
    hostEvent(Ev("start").kv("tool", kToolVersion).kv("runtime", runtime).kv("sdk", kSdkVersion)
              .kv("url", g_opt.url).kv("placement", placement).kn("x", wr.left).kn("y", wr.top)
              .kn("w", wr.right - wr.left).kn("h", wr.bottom - wr.top).kraw("monitors", monitors).kv("udf", g_opt.udf)
              .kv("browserArgs", g_opt.browserArgs).kn("timeout", g_opt.timeoutMs).kn("pid", ::GetCurrentProcessId()), true);

    ::SetTimer(g_hwnd, T_TIMEOUT, g_opt.timeoutMs, nullptr);
    createEnv();

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    g_receivers.clear();
    g_wv.Reset();
    g_ctl.Reset();
    g_env.Reset();
    ::EnterCriticalSection(&g_outLock);
    if (g_outFile != INVALID_HANDLE_VALUE) { ::CloseHandle(g_outFile); g_outFile = INVALID_HANDLE_VALUE; }
    ::LeaveCriticalSection(&g_outLock);
    ::CoUninitialize();
    return g_exitCode;
}
