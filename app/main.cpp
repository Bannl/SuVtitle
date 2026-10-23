// =============================================================================
// SuVtitle - app/main.cpp   (SuVtitle.exe: the window -- Win32 + WebView2)
//
// PORTED 2026-10-23 from AudioPB (PatchBus repo, ui/main.cpp at commit 641f1aa, branch APB_TRANSLATOR "v1.0.6") for
// the SuVtitle split (docs/SPLIT_PLAN.md S2/S6). AudioPB's main.cpp was a tray app with TWO windows: the desk (the
// patchbay) and the Translator window, whose File tab was the player. SuVtitle is that player as a NORMAL Windows app:
// ONE window -- the old Translator window's code, verbatim wherever it could be -- with a taskbar button and no tray
// icon; closing it saves its state, stops the engine and quits.
//
// Kept from 641f1aa: the WebView2 environment WITH the player's switches (autoplay, video overlays off, AudioVideoTracks
// -- and NEVER --disable-direct-composition, see createWebViewEnv), the fresh no-store serving of our pages
// (serveFreshWebAsset), the lock-down of the view (hardenView: navigation guard, no popup, SmartScreen's reputation
// check off), the window's geometry (now app.txt, AppConfig.h), FULL SCREEN = the window (WE D3), always-on-top, drag
// and drop (the page's, with the browser's File objects), the single instance, `--play` (Explorer's "Open with", the
// default player) forwarded by WM_COPYDATA and batched, --unregister-player, and the --test-instance machinery the
// e2e kits drive (an isolated profile or exit 3, its own names and registry root, never the foreground).
// Changed (what the user or Windows sees, S3): the exe, the title "SuVtitle", the class / mutex "SuVtitle.*", the
// origin https://suvtitle.local/, %APPDATA%\SuVtitle + %LOCALAPPDATA%\SuVtitle (the WebView2 profile there), the page
// web\index.html, --register-player (new), and the FIRST-RUN IMPORT of AudioPB's player data (AudioPBImport.h).
// Gone with the desk: the tray icon and its menu, autostart, the panic hotkey, presets, the fabric version check, the
// legacy PatchBus->AudioPB migration.
// =============================================================================
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl.h>
#include <algorithm>   // WG: the --play batch's natural order
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>   // WE: the environment's browser switches (the autoplay rule)
#include "TrLaunch.h"                     // WG: --play, --test-instance, WM_COPYDATA
#include "TrAssoc.h"                      // WG: --unregister-player / --register-player
#include "TrFileStore.h"                  // readFileAll / writeFileAvSafe (app.txt)
#include "AppConfig.h"                    // app.txt: the window's geometry
#include "AudioPBImport.h"                // the first-run import of AudioPB's player data
#include "bridge.h"
#include "resource.h"                     // IDI_APPICON

#pragma comment(lib, "Shlwapi.lib")
#pragma comment(lib, "Shell32.lib")

using namespace Microsoft::WRL;

namespace {

// The names Windows sees (S3). A test instance suffixes the class and the mutex with its tag (TrLaunch.h suffixed).
constexpr wchar_t kWindowClass[] = L"SuVtitle.MainWindow";
constexpr wchar_t kMutexName[]   = L"SuVtitle.SingleInstance";
constexpr wchar_t kWindowTitle[] = L"SuVtitle";
// The page's origin: an internal name never shown to the user, kept stable because it is also the WebView2
// localStorage origin (the subtitle style, the volume, the panel... survive across builds). AudioPB's was
// https://patchbus.local/ -- a different origin, so SuVtitle's page starts with its own defaults.
constexpr wchar_t kWebHost[] = L"https://suvtitle.local/";

constexpr UINT_PTR kUiTickTimer    = 1;            // 1 Hz: Bridge::uiTick (the engine's safety-net drain, the player's clock)
constexpr UINT_PTR kPlayBatchTimer = 2;            // the --play batch closes 300 ms after the last arrival
constexpr UINT_PTR kPlayRetryTimer = 3;            // a file dialog is up: try again in 400 ms
// WG: paths to play -- from this process's own --play, or forwarded by another one through WM_COPYDATA -- wait here
// until the view is attached; then kWmPlayPending hands them to the bridge (which checks every one).
constexpr UINT kWmPlayPending = WM_APP + 0x54;     // (0x51 kWmBridge, 0x52 kWmTranslatorWindow, 0x53 kWmTranslatorPoll)

HWND                             g_hwnd = nullptr;
ComPtr<ICoreWebView2Controller>  g_controller;
ComPtr<ICoreWebView2>            g_webview;
ComPtr<ICoreWebView2Environment> g_environment;   // needed to mint web resource responses
std::unique_ptr<patchbus::Bridge> g_bridge;       // built in wWinMain, in the process that IS the app only

bool            g_quiet = false;                  // a test instance: never the foreground, never activated
bool            g_topmost = false;
// FULL SCREEN IS THE WINDOW (WE D3): borderless on its monitor, and back exactly where it was.
bool            g_fullscreen = false;
LONG_PTR        g_savedStyle = 0;
WINDOWPLACEMENT g_savedPlacement{ sizeof(WINDOWPLACEMENT) };
// One host-initiated NavigateToString (the "page is missing" notice) is allowed past the navigation guard.
bool g_allowStringNav = false;
std::vector<std::wstring> g_pendingPlay;
bool g_playRequested = false;                      // --play with no path at all: the player, empty
// Review of WG #12: Explorer's "Open" on SEVERAL files starts one process per file; each hands its path over. They
// are gathered for a moment (one batch: the first plays, the rest are queued, in the folder's natural order -- not
// whichever forwarder arrived last) -- and held while a file dialog of the player is up (#11).
int g_playSources = 0;

void setFullscreen(bool on);

// Resolve <exe dir>\web as the content root.
std::wstring webContentRoot() {
    wchar_t exe[MAX_PATH];
    ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
    ::PathRemoveFileSpecW(exe);
    std::wstring root = exe;
    root += L"\\web";
    return root;
}

// ----------------------------------------------------------------------------
// Fresh, UNCACHEABLE web serving -- the ONE structural fix for "I rebuilt but the UI is the old version". Every
// request for our virtual host is answered by reading the file straight off disk and returning it with
// `Cache-Control: no-store`, so the WebView2 HTTP cache can never retain OR serve a stale asset.
// ----------------------------------------------------------------------------
const wchar_t* webMimeFor(const std::wstring& name) {
    auto ends = [&](const wchar_t* ext) {
        const size_t n = ::wcslen(ext);
        return name.size() >= n && ::_wcsicmp(name.c_str() + name.size() - n, ext) == 0;
    };
    if (ends(L".html"))  return L"text/html";
    if (ends(L".js"))    return L"text/javascript";
    if (ends(L".css"))   return L"text/css";
    if (ends(L".svg"))   return L"image/svg+xml";
    if (ends(L".png"))   return L"image/png";
    if (ends(L".json"))  return L"application/json";
    // SuVtitle: the font and image types a page may carry (AudioPB served its woff2 as octet-stream -- Chromium is
    // lenient for fonts, but a declared type costs nothing)
    if (ends(L".woff2")) return L"font/woff2";
    if (ends(L".woff"))  return L"font/woff";
    if (ends(L".ico"))   return L"image/x-icon";
    if (ends(L".jpg") || ends(L".jpeg")) return L"image/jpeg";
    if (ends(L".webp"))  return L"image/webp";
    return L"application/octet-stream";
}

// A request path -> a file under web\, or "" (the handler declines). AudioPB's took the LAST segment only (its web\
// was flat); SuVtitle accepts sub-folders, every segment checked by AppConfig.h webRelPath (proved in app_smoke): no
// path can climb out of web\, name a device, an alternate stream or an absolute path.
std::wstring webFileFor(const std::wstring& path) {
    const std::wstring rel = pbapp::webRelPath(path);
    return rel.empty() ? std::wstring() : webContentRoot() + L"\\" + rel;
}

// RV2-4 (2026-10-23): the Content-Security-Policy of everything this handler serves, fitted to index.html -- the one
// page (S-WEB's, after its fix round): its ONE inline <script> and its inline <style> ('unsafe-inline': one local page,
// no nonce machinery), its title font from this origin, the chevrons its CSS draws as data: SVG, and the media of the
// hosts the bridge maps -- f<folderKey>.suvtitle-media.example (the film's folder) and suvtitle-proxy.example (the
// sound-track copy). Nothing else: no fetch / XHR / WebSocket (connect-src falls back to 'none'), no frame, worker,
// plugin, <base> or form -- and NO blob:, which only the page's design mock makes (a plain browser, never this host).
// tests\e2e\real runs the real page under it (R19: this exact header; R*: no violation in the whole run).
constexpr wchar_t kWebCsp[] =
    L"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; font-src 'self'; img-src data:; "
    L"media-src https://*.suvtitle-media.example https://suvtitle-proxy.example; base-uri 'none'; form-action 'none'; "
    L"frame-ancestors 'none'; object-src 'none'";

// A page asset off disk, whole (64 MB at most: a page asset, not a film). False = there is no such file.
bool readWebFile(const std::wstring& file, std::vector<BYTE>& buf) {
    HANDLE h = ::CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!::GetFileSizeEx(h, &sz) || sz.QuadPart < 0 || sz.QuadPart > (64LL << 20)) { ::CloseHandle(h); return false; }
    buf.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    const bool ok = buf.empty() || (::ReadFile(h, buf.data(), (DWORD)buf.size(), &rd, nullptr) && rd == (DWORD)buf.size());
    ::CloseHandle(h);
    return ok;
}

// One answer of ours: never cached, under the policy above.
bool respondWeb(ICoreWebView2WebResourceRequestedEventArgs* args, int status, const wchar_t* reason,
                const std::vector<BYTE>& body, const wchar_t* mime) {
    IStream* stream = ::SHCreateMemStream(body.empty() ? (const BYTE*)"" : body.data(), (UINT)body.size());
    if (!stream) return false;
    const std::wstring headers = std::wstring(L"Content-Type: ") + mime + L"; charset=utf-8\r\n"
                               + L"Cache-Control: no-store, must-revalidate\r\n"
                               + L"Content-Security-Policy: " + kWebCsp;
    ComPtr<ICoreWebView2WebResourceResponse> resp;
    const HRESULT hr = g_environment->CreateWebResourceResponse(stream, status, reason, headers.c_str(), &resp);
    stream->Release();
    if (FAILED(hr) || !resp) return false;
    args->put_Response(resp.Get());
    return true;
}

// Map a request URI to a file under web/, read it, and answer it uncached. RV2-4 (2026-10-23): a request of OUR host is
// always answered HERE, a miss with a 404 of our own. Before, a miss was DECLINED and WebView2 took the request to the
// network: RV-2's probe saw a DNS lookup of suvtitle.local leave the machine (net::ERR_NAME_NOT_RESOLVED) -- and .local
// is mDNS, so a host of that name on the LAN would have served this page's own origin.
bool serveFreshWebAsset(ICoreWebView2WebResourceRequestedEventArgs* args) {
    if (!g_environment) return false;
    ComPtr<ICoreWebView2WebResourceRequest> req;
    if (FAILED(args->get_Request(&req)) || !req) return false;
    LPWSTR uriRaw = nullptr;
    if (FAILED(req->get_Uri(&uriRaw)) || !uriRaw) return false;
    std::wstring uri(uriRaw);
    ::CoTaskMemFree(uriRaw);

    const std::wstring host = kWebHost;
    if (uri.size() < host.size() || ::_wcsnicmp(uri.c_str(), host.c_str(), host.size()) != 0) return false;   // not our origin
    std::vector<BYTE> buf;
    const std::wstring file = webFileFor(uri.substr(host.size()));
    if (!file.empty() && readWebFile(file, buf)) return respondWeb(args, 200, L"OK", buf, webMimeFor(file));
    static const char kMissing[] = "Not found.";
    const BYTE* m = reinterpret_cast<const BYTE*>(kMissing);
    return respondWeb(args, 404, L"Not Found", std::vector<BYTE>(m, m + sizeof(kMissing) - 1), L"text/plain");
}

// ----------------------------------------------------------------------------
// The lock-down of the view (wave WC, 2026-10-10).
//   * NAVIGATION GUARD. The view may show our own page and NOTHING else: any top-level navigation that is not
//     https://suvtitle.local/ is cancelled, and no popup window is ever created. The view also serves the folder of
//     the film being played (a virtual-host mapping, which is NOT a read barrier -- probe 10/10), so it must never
//     show anything but our own page. A file dropped outside the page's own drop zones would otherwise REPLACE the
//     page with that file.
//   * SmartScreen off (ICoreWebView2Settings8): its reputation check costs about 2 s on EVERY navigation to a .local
//     host (probe 10/10: translator.html 2044 ms -> 59-139 ms), and the page reloads whenever a film from a new folder
//     is opened. Our page is a local file served by our own handler; the guard above keeps it that way.
// `allowStringNav`: one host-initiated NavigateToString -- the "page is missing" notice -- is let through, once.
// ----------------------------------------------------------------------------
void hardenView(ICoreWebView2* wv, bool* allowStringNav) {
    if (!wv) return;
    ComPtr<ICoreWebView2Settings> settings;
    if (SUCCEEDED(wv->get_Settings(&settings)) && settings) {
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        ComPtr<ICoreWebView2Settings8> s8;
        if (SUCCEEDED(settings.As(&s8)) && s8) s8->put_IsReputationCheckingRequired(FALSE);
    }
    EventRegistrationToken navToken{};
    wv->add_NavigationStarting(
        Callback<ICoreWebView2NavigationStartingEventHandler>(
            [allowStringNav](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                LPWSTR raw = nullptr;
                std::wstring uri;
                if (SUCCEEDED(a->get_Uri(&raw)) && raw) { uri = raw; ::CoTaskMemFree(raw); }
                const std::wstring ours = kWebHost;
                if (uri.size() >= ours.size() && ::_wcsnicmp(uri.c_str(), ours.c_str(), ours.size()) == 0)
                    return S_OK;
                if (allowStringNav && *allowStringNav) { *allowStringNav = false; return S_OK; }
                a->put_Cancel(TRUE);
                return S_OK;
            }).Get(),
        &navToken);
    EventRegistrationToken winToken{};
    wv->add_NewWindowRequested(
        Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT {
                a->put_Handled(TRUE);   // handled with no window: window.open() returns null
                return S_OK;
            }).Get(),
        &winToken);
}

// ----------------------------------------------------------------------------
// The window's geometry: %APPDATA%\SuVtitle\app.txt (AppConfig.h). AudioPB kept it as the WINPOS line of
// translator.txt, a file it shared with the helper; app.txt is the app's alone (S11).
// ----------------------------------------------------------------------------
std::wstring appTxtPath() {
    wchar_t ad[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    const std::wstring dir = std::wstring(ad) + L"\\SuVtitle";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\app.txt";
}

bool readWinPos(pbapp::WinPos& p) {
    const std::wstring f = appTxtPath();
    std::string text;
    return !f.empty() && pbtrf::readFileAll(f, text, 1ULL << 20) && pbapp::parseWinPos(text, p);
}

void saveWinPos() {
    if (!g_hwnd) return;
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    // In full screen the window IS the monitor: remember where it was before, never that.
    if (g_fullscreen) wp = g_savedPlacement;
    else if (!::GetWindowPlacement(g_hwnd, &wp)) return;
    const RECT& n = wp.rcNormalPosition;      // the RESTORED rect: reopening minimised is useless
    pbapp::WinPos p;
    p.x = n.left; p.y = n.top; p.w = n.right - n.left; p.h = n.bottom - n.top;
    p.topmost = g_topmost;
    p.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    const std::wstring f = appTxtPath();
    if (f.empty()) return;
    std::string old;
    pbtrf::readFileAll(f, old, 1ULL << 20);
    const std::string body = pbapp::packWinPos(old, p);
    if (body != old) pbtrf::writeFileAvSafe(f, body);   // temp + rename with the antivirus retry (S11)
}

void resizeView() {
    if (!g_controller || !g_hwnd) return;
    RECT rc; ::GetClientRect(g_hwnd, &rc);
    g_controller->put_Bounds(rc);
}

// RV4-7 (2026-10-23): the window's REAL always-on-top state -- WS_EX_TOPMOST as Windows has it -- is what the page's
// pin shows (translatorWindow.topmost) and what app.txt keeps. Not the wish: Windows may refuse a background window's
// topmost (measured 23/10 on a never-activated test window), and another program may set it (PowerToys' Always on Top).
bool realTopmost(HWND h) { return h && (::GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0; }

void setTopmost(bool on) {
    if (!g_hwnd) { g_topmost = on; return; }
    ::SetWindowPos(g_hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_topmost = realTopmost(g_hwnd);        // what Windows made of it
    saveWinPos();
    // told EVERY time, a refusal included: the page's pin flipped when it was clicked and must come back to the truth
    if (g_bridge) g_bridge->translatorTopmostChanged(g_topmost);
}

// The page asked for full screen (F, Esc, its button, a double-click on the picture; or the <video> element's own,
// via ContainsFullScreenElementChanged): the window becomes a borderless rectangle the size of ITS monitor, and comes
// back exactly where it was. Raymond Chen's recipe -- style and placement saved, WS_OVERLAPPEDWINDOW stripped,
// SWP_FRAMECHANGED.
void setFullscreen(bool on) {
    if (!g_hwnd) return;
    // Already so: nothing to do -- but the page that asked is told again, so its idea of the window can never stay
    // out of step with the window itself (WE: the page asks, the window decides).
    if (on == g_fullscreen) { if (g_bridge) g_bridge->translatorFullscreenChanged(g_fullscreen); return; }
    if (on) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(::MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
        g_savedStyle = ::GetWindowLongPtrW(g_hwnd, GWL_STYLE);
        g_savedPlacement.length = sizeof(WINDOWPLACEMENT);
        if (!::GetWindowPlacement(g_hwnd, &g_savedPlacement)) return;
        g_fullscreen = true;
        ::SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_savedStyle & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        ::SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                       SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_quiet ? SWP_NOACTIVATE : 0));
    } else {
        g_fullscreen = false;
        ::SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_savedStyle);
        ::SetWindowPlacement(g_hwnd, &g_savedPlacement);
        ::SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_quiet ? SWP_NOACTIVATE : 0));
    }
    resizeView();
    // The window OWNS this fact; the page is told the outcome -- and a page reloaded for a new folder gets it back on
    // its `ready` (bridge.h).
    if (g_bridge) g_bridge->translatorFullscreenChanged(g_fullscreen);
}

// Up and visible (a --play while minimised, a second launch): restored if minimised, never un-maximised, in front
// unless a test instance (which never takes the user's foreground).
void bringUp() {
    if (!g_hwnd) return;
    if (::IsIconic(g_hwnd)) ::ShowWindow(g_hwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);
    else if (!::IsWindowVisible(g_hwnd)) ::ShowWindow(g_hwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (!g_quiet) ::SetForegroundWindow(g_hwnd);
}

UINT dpiOfWindow(HWND hwnd) {
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    auto getDpi = u32 ? (UINT(WINAPI*)(HWND))::GetProcAddress(u32, "GetDpiForWindow") : nullptr;
    const UINT d = getDpi ? getDpi(hwnd) : 0;
    return d ? d : 96;
}

// ----------------------------------------------------------------------------
// WebView2 creation chain
// ----------------------------------------------------------------------------
void createWebViewEnv(HWND hwnd, const wchar_t* userDataFolder, bool withSwitches);

void createWebView(HWND hwnd) {
    // User-data folder under %LOCALAPPDATA% -- NOT next to the exe. An installed build in Program Files can't write
    // beside the exe, so WebView2's env creation fails ASYNC with ACCESS_DENIED (AudioPB: a silent white window on
    // every real install). Per-user, writable, and any failure surfaced.
    static std::wstring udf;
    {
        wchar_t la[MAX_PATH] = {};
        const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            udf = std::wstring(la) + L"\\SuVtitle";
            ::CreateDirectoryW(udf.c_str(), nullptr);
            udf += L"\\WebView2";
            ::CreateDirectoryW(udf.c_str(), nullptr);
        }
    }
    const wchar_t* userDataFolder = udf.empty() ? nullptr : udf.c_str();
    createWebViewEnv(hwnd, userDataFolder, /*withSwitches=*/true);
}

// The environment, and the view on it. withSwitches:
// `--autoplay-policy=no-user-gesture-required` (WE, WE_PLAN_v2 D2, MEASURED in STEP0 M3).
// WHY IT IS NEEDED: the player starts a film the moment its subtitles are ready ("play when ready"), moves to the next
// file at the end of one, and its page RELOADS whenever a file from a new folder is opened (a folder mapping is
// invisible to a page loaded before it). Chromium's autoplay rule wants a click IN THE PAGE that plays: measured, the
// click that opened the file does not survive that reload (NotAllowedError again).
// WHY IT IS SAFE: the view only ever shows OUR OWN local page (https://suvtitle.local/, served from disk by
// serveFreshWebAsset) behind the navigation guard (hardenView) -- no site from the internet can ever run in it.
// If the environment cannot be created WITH the switches (another process holds this profile with other switches),
// it is created again without them: the player then shows a "Press play" button instead of starting by itself.
// `--disable-direct-composition-video-overlays` (user report 11/10, AudioPB 1.0.6): "the subtitles disappear if I don't
// move the mouse". Nothing in the page hides them on idle; what changes when the pointer stops is the COMPOSITOR: with
// a still scene Chromium promotes the <video> to a hardware overlay plane (DirectComposition / MPO), and on some
// drivers -- AMD's among them -- what the page draws over that hole (our subtitles) is no longer presented.
// (!) NEVER `--disable-direct-composition` (tried 11/10, then REMOVED): it cured the full-screen case of the same bug,
// but it also turns HEVC OFF -- measured 11/10 (wv2probe, the user's RX 7700 XT, runtime 154), every HEVC file
// (mp4/mov/mkv, the user's x265 film included) fails with DECODER_ERROR_NOT_SUPPORTED, H.264/VP9/AV1 unaffected. The
// full-screen case is handled in the PAGE instead (#fileVideo drawn with a blend mode in an isolated group: never
// promoted to a hardware plane; Ctrl+Alt+V cycles the rendering).
// `--enable-blink-features=AudioVideoTracks` (wave WH): HTMLMediaElement.audioTracks, so the player can switch between
// a film's own sound tracks (measured 11/10 on the user's MKV: its 5 AAC tracks listed, switched mid-play).
// (The e2e kits set WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS, which OVERRIDES these: they must repeat all three.)
void createWebViewEnv(HWND hwnd, const wchar_t* userDataFolder, bool withSwitches) {
    ComPtr<ICoreWebView2EnvironmentOptions> opts;
    if (withSwitches) {
        auto o = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
        if (o && SUCCEEDED(o->put_AdditionalBrowserArguments(
                     L"--autoplay-policy=no-user-gesture-required"
                     L" --disable-direct-composition-video-overlays"
                     L" --enable-blink-features=AudioVideoTracks")))
            opts = o;
    }
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder, opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, userDataFolder, withSwitches](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if ((FAILED(r) || !env) && withSwitches) {
                    createWebViewEnv(hwnd, userDataFolder, false);   // once more, without the switches
                    return S_OK;
                }
                if (FAILED(r) || !env) {
                    ::MessageBoxW(hwnd,
                        L"SuVtitle could not start its WebView2 environment.\n\n"
                        L"Check that the Microsoft Edge WebView2 Runtime is installed and that "
                        L"%LOCALAPPDATA%\\SuVtitle is writable.",
                        L"SuVtitle", MB_ICONERROR | MB_OK);
                    ::DestroyWindow(hwnd);
                    return r;
                }
                g_environment = env;   // kept to mint uncached web resource responses
                env->CreateCoreWebView2Controller(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT r2, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(r2) || !controller) {
                                ::MessageBoxW(hwnd, L"SuVtitle could not create its WebView2 view.",
                                              L"SuVtitle", MB_ICONERROR | MB_OK);
                                ::DestroyWindow(hwnd);
                                return r2;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            if (!g_webview) { ::DestroyWindow(hwnd); return E_FAIL; }
                            // FILE DROPS: the page takes them (the user, 11/10: "qu'on puisse drag & drop les fichiers
                            // audio/video dans le lecteur") -- its drop zones hand the File objects to the bridge
                            // (translatorFileDrop: the paths are WebView2's). The navigation guard is the second line.
                            {
                                ComPtr<ICoreWebView2Controller4> c4;
                                if (SUCCEEDED(g_controller.As(&c4)) && c4) c4->put_AllowExternalDrop(TRUE);
                            }
                            // (!) THE staleness fix: serve https://suvtitle.local/ ENTIRELY from the
                            // WebResourceRequested handler, each file fresh from disk with Cache-Control: no-store. We
                            // deliberately do NOT map our own host to web/: a mapped host is served by WebView2
                            // INTERNALLY and bypasses WebResourceRequested (verified on AudioPB), which would defeat the
                            // no-store. (The MEDIA folders ARE mapped -- by the bridge, on other hosts.)
                            g_webview->AddWebResourceRequestedFilter((std::wstring(kWebHost) + L"*").c_str(),
                                                                     COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                            EventRegistrationToken wrToken{};
                            g_webview->add_WebResourceRequested(
                                Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
                                        serveFreshWebAsset(a);
                                        return S_OK;
                                    }).Get(),
                                &wrToken);
                            // Lock down the shell (context menu, devtools, SmartScreen's 2 s) and pin the view to our
                            // own origin -- see hardenView.
                            hardenView(g_webview.Get(), &g_allowStringNav);
                            // FILE MODE (WC): the <video> element's own full screen goes to the whole monitor (see
                            // setFullscreen); the page leaves it by itself (Esc, its button).
                            EventRegistrationToken fsToken{};
                            g_webview->add_ContainsFullScreenElementChanged(
                                Callback<ICoreWebView2ContainsFullScreenElementChangedEventHandler>(
                                    [](ICoreWebView2* sender, IUnknown*) -> HRESULT {
                                        BOOL fs = FALSE;
                                        if (sender && SUCCEEDED(sender->get_ContainsFullScreenElement(&fs)))
                                            setFullscreen(fs != FALSE);
                                        return S_OK;
                                    }).Get(),
                                &fsToken);

                            // The bridge: its messages, the engine, the player's folders mapped BEFORE the first
                            // navigation (a mapping set after a page loaded is invisible to it).
                            if (g_bridge) {
                                g_bridge->setHostWindow(hwnd);
                                g_bridge->attach(g_webview.Get(), hwnd);
                            }
                            // WG: started by --play (Explorer's double-click): the files, now that the view can take them
                            if (g_playRequested || !g_pendingPlay.empty()) ::PostMessageW(hwnd, kWmPlayPending, 0, 0);
                            resizeView();

                            // The page is S-WEB's file. If it has not landed, say so in words instead of showing a
                            // blank window nobody can diagnose.
                            const std::wstring page = webContentRoot() + L"\\index.html";
                            if (::GetFileAttributesW(page.c_str()) == INVALID_FILE_ATTRIBUTES) {
                                g_allowStringNav = true;    // the one navigation the guard lets through
                                g_webview->NavigateToString(
                                    L"<body style=\"margin:0;background:#14161a;color:#c8ccd4;"
                                    L"font:15px system-ui;display:flex;align-items:center;"
                                    L"justify-content:center;height:100vh\">"
                                    L"<div style=\"text-align:center\"><p>web\\index.html is not "
                                    L"installed next to SuVtitle.exe.</p>"
                                    L"<p style=\"opacity:.6\">The SuVtitle engine is running; only "
                                    L"this page is missing.</p></div></body>");
                            } else {
                                g_webview->Navigate((std::wstring(kWebHost) + L"index.html").c_str());
                            }
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr) && withSwitches) { createWebViewEnv(hwnd, userDataFolder, false); return; }
    if (FAILED(hr)) {
        ::MessageBoxW(hwnd,
            L"WebView2 Runtime not found.\n\n"
            L"Install the \"Microsoft Edge WebView2 Runtime\", then start SuVtitle again.",
            L"SuVtitle", MB_ICONERROR | MB_OK);
        // (posted, not DestroyWindow: this can run inside WM_CREATE, before CreateWindowEx has even returned)
        ::PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
}

// ----------------------------------------------------------------------------
// Window procedure
// ----------------------------------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        createWebView(hwnd);
        ::SetTimer(hwnd, kUiTickTimer, 1000, nullptr);   // drives Bridge::uiTick
        return 0;

    case WM_TIMER:
        if (wParam == kPlayBatchTimer || wParam == kPlayRetryTimer) {   // WG: the --play batch is closed / retried
            ::KillTimer(hwnd, wParam);
            ::PostMessageW(hwnd, kWmPlayPending, 0, 0);
            return 0;
        }
        if (wParam == kUiTickTimer && g_bridge) g_bridge->uiTick();
        return 0;

    case WM_SIZE:
        resizeView();
        // a maximise / restore never goes through WM_EXITSIZEMOVE: remembered here (not in full screen: that is
        // the monitor, not the user's window)
        if (!g_fullscreen && (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED)) {
            static WPARAM last = SIZE_RESTORED;
            if (wParam != last) { last = wParam; saveWinPos(); }
        }
        return 0;

    case WM_GETMINMAXINFO: {
        // The player stacks a picture, a transport, a status strip and a subtitle list: below this the page has no
        // room left to be usable (WC).
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = dpiOfWindow(hwnd);
        mmi->ptMinTrackSize.x = (LONG)(420 * dpi / 96);
        mmi->ptMinTrackSize.y = (LONG)(320 * dpi / 96);
        return 0;
    }

    // Per-Monitor-V2 contract: dragging onto a monitor with another scale hands us the window rect Windows wants;
    // taking it (then resizing the WebView) keeps the page pixel-crisp on both screens.
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        resizeView();
        return 0;
    }

    case WM_EXITSIZEMOVE:
        saveWinPos();      // where the user PUT it, remembered when they let go
        return 0;

    // Off-thread bridge posts wake us here to drain onto the WebView.
    case patchbus::Bridge::kWmBridge:
        if (g_bridge) g_bridge->drainWebQueue();
        return 0;

    // RV4-7: always-on-top changed by someone else (PowerToys' Always on Top...): the page's pin and app.txt follow.
    // (Not while the window is being created: g_hwnd is set once CreateWindowExW returns.)
    case WM_WINDOWPOSCHANGED:
        if (g_hwnd == hwnd && realTopmost(hwnd) != g_topmost) {
            g_topmost = !g_topmost;
            saveWinPos();
            if (g_bridge) g_bridge->translatorTopmostChanged(g_topmost);
        }
        break;                                       // (DefWindowProc: WM_SIZE / WM_MOVE)

    // The Bridge owns no window, so it posts its intent here.
    case patchbus::Bridge::kWmTranslatorWindow:
        if (wParam == 0)      bringUp();
        else if (wParam == 1) setTopmost(lParam != 0);
        // WE (D3): FULL SCREEN IS THE WINDOW -- the page asks (F, Esc, its button, a double-click on the picture) and
        // the window goes borderless on its monitor, or comes back.
        else if (wParam == 2) setFullscreen(lParam != 0);
        return 0;

    // The engine has something to say (W1), or a scan / probe / extraction finished: one message pump away.
    case patchbus::Bridge::kWmTranslatorPoll:
        if (g_bridge) g_bridge->pumpTranslator();
        return 0;

    // WG: ANOTHER SuVtitle was started with --play (Explorer's double-click on a video, "Open with"): it hands the
    // paths over and quits -- one SuVtitle, ever. Anything malformed is refused whole (TrLaunch.h unpackPlay); the
    // paths are only queued here and checked one by one by the bridge (playPaths), off this SendMessage.
    case WM_COPYDATA: {
        const auto* cds = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        std::vector<std::wstring> paths;
        if (!cds || cds->dwData != pblaunch::kCopyPlay || !pblaunch::unpackPlay(cds->lpData, cds->cbData, paths)) return FALSE;
        g_pendingPlay.insert(g_pendingPlay.end(), paths.begin(), paths.end());
        g_playRequested = true;
        ++g_playSources;
        if (g_webview) ::SetTimer(hwnd, kPlayBatchTimer, 300, nullptr);   // (else: when the view is attached)
        return TRUE;
    }
    case kWmPlayPending: {
        if (!g_webview || !g_bridge || !g_playRequested) return 0;
        if (g_bridge->dialogBusy()) { ::SetTimer(hwnd, kPlayRetryTimer, 400, nullptr); return 0; }   // #11: after the dialog
        std::vector<std::wstring> paths;
        paths.swap(g_pendingPlay);
        g_playRequested = false;
        // several processes fed this batch: the natural order (Explorer's own: "Episode 2" < "Episode 10"), not their
        // arrival order; one process's own list keeps its order
        if (g_playSources > 1)
            std::stable_sort(paths.begin(), paths.end(),
                             [](const std::wstring& a, const std::wstring& b) { return ::StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
        g_playSources = 0;
        bringUp();                       // the player, in front (the user asked for it) -- a test instance never
        g_bridge->showPlayer();
        g_bridge->playPaths(paths);
        return 0;
    }

    // Closing the window QUITS (SuVtitle is a normal app, S2): out of full screen first (the geometry saved is the
    // user's window, not the monitor), the geometry written, then WM_DESTROY saves the player and stops the engine.
    case WM_CLOSE:
        if (g_fullscreen) setFullscreen(false);
        saveWinPos();
        ::DestroyWindow(hwnd);
        return 0;

    // The Windows SESSION is ending (logoff, shutdown, restart): the process may be killed without a WM_DESTROY, so
    // the player's state is written NOW -- its queue, the positions, the vouched part of a running analysis (review
    // of WE, P8). Nothing is stopped: the end may still be cancelled.
    case WM_QUERYENDSESSION:
        saveWinPos();
        if (g_bridge) g_bridge->persistPlayer();
        return TRUE;
    case WM_ENDSESSION:
        if (wParam && g_bridge) g_bridge->persistPlayer();
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd, kUiTickTimer);
        if (g_bridge) g_bridge->shutdown();   // the player saved, the engine stopped (QUIT, then forced)
        if (g_controller) g_controller->Close();
        g_hwnd = nullptr;
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    // Per-Monitor-V2 DPI awareness, FIRST (before any window exists): without it Windows bitmap-stretches the whole
    // window on a scaled display and the WebView renders blurry.
#ifdef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#else
    ::SetProcessDPIAware();
#endif

    // WG: the command line (TrLaunch.h). isTestInstance() is the FIRST question: a test instance whose %APPDATA% or
    // %LOCALAPPDATA% is the real one exits here (3), before anything is read or written.
    const pblaunch::Args& args = pblaunch::processArgs();
    const bool testInstance = pblaunch::isTestInstance();
    g_quiet = testInstance;
    const std::wstring regRoot = testInstance ? L"Software\\SuVtitle-Test\\" + pblaunch::testTag() : std::wstring(L"Software");
    // --unregister-player (the uninstaller) removes what the app registered for this user and quits; --register-player
    // (SuVtitle, new) registers it and quits -- no window, no engine, nothing else. A test instance works under its
    // own root, which nothing of the shell reads (no broadcast either).
    if (args.unregister) {
        pbassoc::unregister(HKEY_CURRENT_USER, regRoot);
        if (!testInstance) pbassoc::notifyShell();
        return 0;
    }
    if (args.registerPlayer) {
        wchar_t exe[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring err;
        if (!exe[0] || !pbassoc::apply(HKEY_CURRENT_USER, pbassoc::entries(exe, regRoot), err)) return 1;
        if (!testInstance) pbassoc::notifyShell();
        return 0;
    }
    const std::wstring mutexName = pblaunch::suffixed(kMutexName);
    const std::wstring mainClass = pblaunch::suffixed(kWindowClass);

    // Single instance: one SuVtitle, ever -- a second launch brings the first one up (and hands it its --play paths).
    HANDLE hSingle = ::CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!hSingle) return 1;
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        // The winner may not have created its window yet (near-simultaneous launch): retry briefly.
        HWND existing = nullptr;
        for (int i = 0; i < 20 && !existing; ++i) {
            existing = ::FindWindowW(mainClass.c_str(), nullptr);
            if (!existing) ::Sleep(150);
        }
        // WG: started by Explorer (--play): the paths go to the SuVtitle already running (WM_COPYDATA, TrLaunch.h),
        // in front -- this process passes it its right to the foreground (never a test instance's).
        if (args.play) {
            bool sent = false;
            if (existing) {
                std::vector<wchar_t> buf = pblaunch::packPlay(args.paths);
                COPYDATASTRUCT cds{};
                cds.dwData = pblaunch::kCopyPlay;
                cds.cbData = static_cast<DWORD>(buf.size() * sizeof(wchar_t));
                cds.lpData = buf.data();
                DWORD pid = 0;
                ::GetWindowThreadProcessId(existing, &pid);
                if (pid && !testInstance) ::AllowSetForegroundWindow(pid);
                DWORD_PTR res = 0;
                sent = ::SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cds),
                                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &res) != 0 && res == TRUE;
            }
            ::CloseHandle(hSingle);
            return sent ? 0 : 2;
        }
        if (existing && !testInstance) {
            // not ShowWindow(SW_RESTORE) on a window that is not minimised: it would un-maximise it
            if (::IsIconic(existing)) ::ShowWindow(existing, SW_RESTORE);
            ::SetForegroundWindow(existing);
        }
        ::CloseHandle(hSingle);
        return 0;
    }

    // WebView2 + the shell's dialogs use apartment-threaded COM.
    if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) { ::CloseHandle(hSingle); return 1; }

    // THE FIRST-RUN IMPORT (S4): AudioPB's player data copied over once, BEFORE the Bridge reads any of it. Never
    // fatal; its marker file is its log. (A test instance imports from its own isolated profile.)
    {
        const pbimport::Result imp = pbimport::run(pbimport::productRoots());
        if (imp.ran) {
            wchar_t line[200];
            std::swprintf(line, 200, L"SuVtitle: AudioPB import -- copied %d, kept %d, failed %d\n",
                          imp.copied, imp.kept, imp.failed);
            ::OutputDebugStringW(line);
        }
    }
    g_bridge = std::make_unique<patchbus::Bridge>();

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = mainClass.c_str();
    wc.hIcon   = ::LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));   // title bar + Alt-Tab + taskbar
    wc.hIconSm = (HICON)::LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                     ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    ::RegisterClassExW(&wc);

    // The geometry the user left it in (app.txt) -- unless that rectangle is on no monitor any more (a second screen
    // unplugged): then the default place. Under Per-Monitor-V2 these are PHYSICAL pixels: the default is scaled by the
    // system DPI. AudioPB's translator window opened at 760 x 560 (CSS px) by default: SuVtitle is the player itself,
    // so it opens larger (1100 x 720).
    UINT dpi = 96;
    {
        HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
        auto getDpi = u32 ? (UINT(WINAPI*)())::GetProcAddress(u32, "GetDpiForSystem") : nullptr;
        if (getDpi) dpi = getDpi();
    }
    pbapp::WinPos pos;
    bool haveSaved = readWinPos(pos);
    if (haveSaved) {
        const RECT r{ pos.x, pos.y, pos.x + pos.w, pos.y + pos.h };
        if (!::MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) haveSaved = false;
    }
    g_topmost = haveSaved && pos.topmost;
    g_hwnd = ::CreateWindowExW((g_topmost ? WS_EX_TOPMOST : 0) | (g_quiet ? WS_EX_NOACTIVATE : 0),
                               mainClass.c_str(), kWindowTitle, WS_OVERLAPPEDWINDOW,
                               haveSaved ? pos.x : CW_USEDEFAULT, haveSaved ? pos.y : CW_USEDEFAULT,
                               haveSaved ? pos.w : (int)(1100 * dpi / 96), haveSaved ? pos.h : (int)(720 * dpi / 96),
                               nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) { g_bridge.reset(); ::CoUninitialize(); ::CloseHandle(hSingle); return 1; }
    g_topmost = realTopmost(g_hwnd);                 // RV4-7: the state the page's first `ready` is told
    g_bridge->translatorTopmostChanged(g_topmost);

    // WG: started by Explorer to PLAY a file: the player comes up with it (once the view exists: kWmPlayPending).
    if (args.play) { g_pendingPlay = args.paths; g_playRequested = true; g_playSources = 1; }
    // A test instance: shown, never activated (WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE), never maximised by a saved state.
    int show = nCmdShow;
    if (g_quiet) show = SW_SHOWNOACTIVATE;
    else if (haveSaved && pos.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW)) show = SW_SHOWMAXIMIZED;
    ::ShowWindow(g_hwnd, show);
    ::UpdateWindow(g_hwnd);

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    g_bridge.reset();          // (already shut down in WM_DESTROY; its destructor is a no-op then)
    g_controller.Reset();
    g_webview.Reset();
    g_environment.Reset();
    ::CoUninitialize();
    ::CloseHandle(hSingle);
    return static_cast<int>(msg.wParam);
}
