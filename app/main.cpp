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
//
// SUVTITLE LIVE (1.1.0, 2026-10-25 -- docs/LIVE_PLAN.md section 4.3; AudioPB Translator 1.1.0's TrWndProc /
// openTranslatorWindow, commit 50f79d6): a SECOND window, the Live window -- live.html in its own WebView2 controller on
// the SAME environment, locked down like the player's (hardenView, no drop, its own CSP: no media), always-on-top with
// the REAL state told to its page, its place in app.txt (LIVEPOS, clamped onto a monitor, seeded once from AudioPB's
// translator.txt), the Save dialog owned by it. The app's life changes with it: it quits when its LAST window closes
// (the player only HIDES while the Live window is open; closing the Live window stops listening and hides it), and
// `--live` -- the "SuVtitle Live" shortcut -- opens the Live window alone (the player's window is created, the app's hub,
// but never shown), forwarded to a running SuVtitle like --play. Both views reload by themselves after a renderer crash
// (ProcessFailed), and are rebuilt if the browser process itself dies: a two-hour meeting must not end on a dead window.
//
// 1.1.0 THE NATIVE VIDEO ENGINE (N1, 2026-10-26 -- docs/NATIVE_CONTRACT.md, NATIVE_ENGINE_PLAN.md section 2; ported from
// the N0 prototype scratchpad\n0\src\n0proto.cpp, which PASSED on the user's own screen): mpv plays every file in a
// disabled child window (MpvPlayer.h), and the player's page is VISUAL-HOSTED over it -- a composition controller whose
// visual sits in a DirectComposition target created TOPMOST on this window, so DWM draws the page ABOVE the child; the page
// is transparent where the picture is. What visual hosting moves to the host lives in the block "THE NATIVE VIDEO ENGINE"
// below: mouse / wheel / pen / touch forwarded, the page's cursor (with the page's nativeCursor safety net), the keyboard
// focus, the scale of the picture's maths, file drops forwarded to the page's own drop zones, UI Automation, mpv started
// once the window exists and ended before it. The Live window keeps its WINDOWED view on the same environment. When this
// runtime or DirectComposition cannot host visually, the player's view is 1.0's windowed one and the page plays everything
// with its own <video> (the bridge is told why).
// =============================================================================
#include <windows.h>
#include <windowsx.h>                     // (N1) GET_X_LPARAM: the forwarded mouse
#include <ole2.h>                         // (N1) OleInitialize, RegisterDragDrop: the forwarded drops
#include <shellapi.h>
#include <shlobj.h>                       // (N1) IDropTargetHelper, the test instance's drop
#include <shlwapi.h>
#include <dcomp.h>                        // (N1) DirectComposition: the page's visual, TOPMOST over mpv's picture
#include <uiautomation.h>                 // (N1) the page's UI Automation tree, from the host's window
#include <wrl.h>
#include <algorithm>   // WG: the --play batch's natural order
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>   // WE: the environment's browser switches (the autoplay rule)
#include "MpvPlayer.h"                    // (N1) the native video engine: libmpv under the page
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

// ---- 1.1.0 THE NATIVE VIDEO ENGINE (N1): the player's page over mpv's picture (the block of that name below) ----------
constexpr UINT kWmNativeStart = WM_APP + 0x56;     // mpv starts once the window exists (posted by WM_CREATE)
constexpr UINT kWmTestDrop    = WM_APP + 0x57;     // a TEST INSTANCE's drop, off the WM_COPYDATA that asked for it
constexpr ULONG_PTR kCopyTestDrop = 0x53564454;    // 'SVDT': WM_COPYDATA of a TEST INSTANCE only -- a file to drop
std::unique_ptr<suvmpv::MpvPlayer> g_native;      // libmpv (MpvPlayer.h); created by startNative()
bool        g_nativeTried = false;                 // startNative() ran, whatever came of it
std::string g_nativeWhy;                           // why mpv is not usable ("" = it is)
bool        g_dllLocked = false;                   // (fix round MEDIUM4) wWinMain's lockDllSearch() held: native.log says so
ComPtr<ICoreWebView2CompositionController> g_compController;   // the player's view when VISUAL-hosted (null = windowed)
ComPtr<IDCompositionDevice> g_dcomp;               // the page's visual: a TOPMOST target on the player's window
ComPtr<IDCompositionTarget> g_dcompTarget;
ComPtr<IDCompositionVisual> g_dcompRoot, g_dcompView;
std::wstring g_testDropPath;                       // (a test instance's drop, between WM_COPYDATA and kWmTestDrop)
bool initComposition(HWND hwnd);
void setupVisualHosting();
void attachNativeToBridge();
void startNative();
void updateNativeScale();
bool forwardMouse(UINT msg, WPARAM wParam, LPARAM lParam);
bool forwardPointer(UINT msg, WPARAM wParam, LPARAM lParam);
HCURSOR cursorToSet();
void pageCursorWord(bool show);
void focusView();
LRESULT automationObject(HWND hwnd, WPARAM wParam, LPARAM lParam);
void testDrop();
void endNative();

// ---- SUVTITLE LIVE (1.1.0): the Live window ---------------------------------------------------------------------------
// A second top-level window (its own taskbar button), never owned by the player's: the player may be hidden -- or never
// shown, under --live -- while it is up. Created on the first request, then HIDDEN on close, never destroyed until the app
// ends: reopening is instant and its page keeps its scrollback (the bridge replays 200 lines anyway).
constexpr wchar_t kLiveWindowClass[] = L"SuVtitle.LiveWindow";   // (+ ".<tag>" for a test instance, like the player's)
constexpr wchar_t kLiveWindowTitle[] = L"SuVtitle Live";
// The browser process died (ProcessFailed): both views are rebuilt, off the event (posted).
constexpr UINT kWmRebuildViews = WM_APP + 0x55;
HWND                            g_liveHwnd = nullptr;
ComPtr<ICoreWebView2Controller> g_liveController;
ComPtr<ICoreWebView2>           g_liveWebview;
bool g_liveTopmost = false;            // its REAL WS_EX_TOPMOST (RV4-7, like the player's)
bool g_liveAllowStringNav = false;     // its own one-shot pass for the "page is missing" notice
bool g_liveRequested = false;          // asked before the player's view exists (--live at start): opened right after it
bool g_liveCreating = false;           // its controller is on its way (one at a time)
bool g_showMaxOnce = false;            // the player's saved state is MAXIMIZED and it has not been shown yet (--live)
void openLiveWindow();
void setLiveTopmost(bool on);
void saveLivePos();
void closeLiveWindow();
void hidePlayerWindow();
void rebuildViews(unsigned gen);
void watchProcessFailed(ICoreWebView2* wv, bool live);
// ---- SUVTITLE LIVE fix round (2026-10-26: docs/REVIEW_RV-LB.md RVB-3 / RVB-4, LIVE_PLAN.md "ADDENDUM FIX ROUND") ----------
// * THE LAST WINDOW while the engine is still live (listening, or FINISHING its queued sentences after LIVE 0): the
//   windows hide at once, LIVE 0, the app waits for STATE live 0 (kQuitDrainMs at most), THEN ends -- QUIT does not drain.
//   A relaunch meanwhile brings it back (cancelQuit) instead of racing it.
// * RECOVERY never activates anything (RVB-3: the user games on this machine): after the browser process died the Live
//   window's VIEW is re-created (g_liveViewOwed), never the window shown, restored or brought to the front.
// * A dead view is never dead for good (RVB-4): a reload the per-minute limiter refused is OWED (a timer); a rebuild the
//   5-minute limiter refused, or one that failed, is tried again later -- never the startup's MessageBox-and-quit (the
//   engine and the journal live on: the windows say so meanwhile); a stale creation callback does nothing (g_liveGen);
//   the user reopening the Live window recovers its view at once.
constexpr UINT_PTR kQuitWaitTimer   = 4;       // the drain being waited for (polled)
constexpr UINT_PTR kRebuildTimer    = 5;       // a rebuild owed (the limiter) or to try again (it failed)
constexpr UINT_PTR kReloadTimer0    = 6;       // the player's view: a reload owed
constexpr UINT_PTR kReloadTimer1    = 7;       // the Live view's
constexpr UINT_PTR kLiveViewTimer   = 8;       // the Live view's re-creation after a recovery failed: tried again
constexpr ULONGLONG kQuitDrainMs    = 15000;   // the ADDENDUM: wait for live 0 at most this long
bool      g_quitting = false;                  // the last window closed: hidden, waiting for the drain
ULONGLONG g_quitDeadline = 0;
bool      g_rebuilding = false;                // a rebuild's environment / views are on their way
bool      g_rebuildRetry = false;              // kRebuildTimer = a failed rebuild tried again (not the limiter's owed one)
int       g_rebuildFails = 0;                  // in a row (the retry's back-off)
bool      g_viewsLost = false;                 // the browser died: the views are gone until a rebuild brings them back
bool      g_liveViewOwed = false;              // the Live window's view to re-create after the rebuild -- not the window
int       g_liveViewTries = 0;                 // a recovery's re-creation of it that failed, in a row
unsigned  g_liveGen = 0;                       // the Live view's creation generation (a stale callback does nothing)
// The ENVIRONMENT's generation (MEASURED 26/10 under a debugger on the real run): the browser's death is notified by EACH
// view (two posted rebuilds); the second used to arrive after the first rebuild's environment callback -- the new
// environment set, its controller still on its way -- and started a SECOND rebuild: two controllers came back, the second
// replaced the first under the bridge, which kept the released view -> postToWeb through a freed object (0xC0000005 in
// Bridge::postToWeb). Every notice and every creation callback carries the generation it belongs to; an older one is
// ignored (a controller it made is closed).
unsigned  g_envGen = 0;
ULONGLONG g_reloads[2][3] = {};                // the last three reloads of each view (0 the player's, 1 the Live view's)
bool      g_reloadOwed[2] = {};
ULONGLONG g_rebuilds[3] = {};                  // the last three rebuilds
void quitWhenDrained();
void cancelQuit();
void createLiveView(bool user);
void reloadView(int v, ICoreWebView2* wv);
void rebuildFailed();
bool paintNoView(HWND hwnd);

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
// SUVTITLE LIVE (1.1.0): a policy PER PAGE (AudioPB's WebServe.h idea, LIVE_PLAN 4.3). The Live window's page shows text:
// the player's policy minus media-src -- no film, no sound copy, no media host of any kind for it.
constexpr wchar_t kLiveCsp[] =
    L"default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; font-src 'self'; img-src data:; "
    L"base-uri 'none'; form-action 'none'; frame-ancestors 'none'; object-src 'none'";
constexpr wchar_t kLivePage[] = L"live.html";

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
// SUVTITLE LIVE: `livePage` = the Live window's page, under its own policy (kLiveCsp); everything else is the player's.
bool respondWeb(ICoreWebView2WebResourceRequestedEventArgs* args, int status, const wchar_t* reason,
                const std::vector<BYTE>& body, const wchar_t* mime, bool livePage = false) {
    IStream* stream = ::SHCreateMemStream(body.empty() ? (const BYTE*)"" : body.data(), (UINT)body.size());
    if (!stream) return false;
    std::wstring headers = std::wstring(L"Content-Type: ") + mime + L"; charset=utf-8\r\n"
                         + L"Cache-Control: no-store, must-revalidate\r\n";
    if (livePage) headers += std::wstring(L"Content-Security-Policy: ") + kLiveCsp;
    else          headers = headers + L"Content-Security-Policy: " + kWebCsp;
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
    // SUVTITLE LIVE: the policy is the PAGE's -- live.html gets kLiveCsp (no media), whatever view asked for it
    const bool livePage = ::_wcsicmp(pbapp::webRelPath(uri.substr(host.size())).c_str(), kLivePage) == 0;
    if (!file.empty() && readWebFile(file, buf)) return respondWeb(args, 200, L"OK", buf, webMimeFor(file), livePage);
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
    // SUVTITLE LIVE: a HIDDEN player (--live, or closed while the Live window stays) is where it was when it was last
    // shown -- and its placement then says nothing of a maximized state: the last visible save stands.
    if (!::IsWindowVisible(g_hwnd)) return;
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
    // SUVTITLE LIVE: app.txt holds the Live window's lines too (LIVEPOS, the strips' LIVEIN) -- AN EMPTY READ IS NOT AN
    // EMPTY FILE (AudioPB 1.1.0's lesson on translator.txt): a file that exists but could not be read is not rebuilt
    // from nothing; this save is skipped (one lost position is nothing, lost strips are a bug report).
    if (!pbtrf::readFileAll(f, old, 1ULL << 20) && ::GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) return;
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
        // (N1: the stage is the whole page in full screen -- the picture fills the screen at once, WM_SIZE, before the
        // page's next nativeRect confirms it)
        if (g_native) g_native->setFullscreenHint(true);
        ::SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_savedStyle & ~(LONG_PTR)WS_OVERLAPPEDWINDOW);
        ::SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                       mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                       SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_quiet ? SWP_NOACTIVATE : 0));
    } else {
        g_fullscreen = false;
        if (g_native) g_native->setFullscreenHint(false);   // (N1: the windowed picture's place comes back at once)
        ::SetWindowLongPtrW(g_hwnd, GWL_STYLE, g_savedStyle);
        ::SetWindowPlacement(g_hwnd, &g_savedPlacement);
        ::SetWindowPos(g_hwnd, g_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | (g_quiet ? SWP_NOACTIVATE : 0));
        // (fix round LOW10: while the browser is gone -- a rebuild on its way -- the picture stays hidden: the windowed
        // rectangle just restored would show the paused film over the "display stopped" notice)
        if (g_native && g_viewsLost) g_native->place(RECT{});
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
    cancelQuit();          // (fix round: the app was ending -- its last window closed, a drain waited for: it is wanted again)
    // SUVTITLE LIVE: the player may be HIDDEN (--live, or closed while the Live window stays): shown again, its view
    // visible again (a hidden one is throttled), its page and the player told (bridge: playerWindowShown) -- and
    // maximized the first time if that is how the user left it (a --live start never showed it).
    const bool wasHidden = !::IsWindowVisible(g_hwnd);
    if (::IsIconic(g_hwnd)) ::ShowWindow(g_hwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);
    else if (wasHidden) ::ShowWindow(g_hwnd, g_quiet ? SW_SHOWNOACTIVATE : (g_showMaxOnce ? SW_SHOWMAXIMIZED : SW_SHOW));
    g_showMaxOnce = false;
    if (wasHidden) {
        if (g_controller) g_controller->put_IsVisible(TRUE);
        if (g_bridge) g_bridge->playerWindowShown(true);
    }
    if (!g_quiet) ::SetForegroundWindow(g_hwnd);
}

// SUVTITLE LIVE (1.1.0): the player's window closed while the Live window is open -- it HIDES (the app quits with its
// last window): its page is told so and pauses what it plays (translatorWindow{shown:false}), its view goes invisible
// (WebView2 throttles a page nobody can see), the pre-analysis waits. bringUp() brings it back.
void hidePlayerWindow() {
    if (!g_hwnd) return;
    ::ShowWindow(g_hwnd, SW_HIDE);
    if (g_controller) g_controller->put_IsVisible(FALSE);
    if (g_bridge) g_bridge->playerWindowShown(false);
}

UINT dpiOfWindow(HWND hwnd) {
    HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
    auto getDpi = u32 ? (UINT(WINAPI*)(HWND))::GetProcAddress(u32, "GetDpiForWindow") : nullptr;
    const UINT d = getDpi ? getDpi(hwnd) : 0;
    return d ? d : 96;
}

// =============================================================================================================
// 1.1.0 THE NATIVE VIDEO ENGINE (N1, 2026-10-26) -- the host's half of visual hosting. Ported from the N0 prototype
// (scratchpad\n0\src\n0proto.cpp: initDComp, forwardMouse, forwardPointer, onCursorChanged, the "cursor show" net, the
// focus) with the product's rules: a TEST INSTANCE never captures the user's mouse, never hides his cursor and never makes
// a sound; nothing here runs for the windowed fallback.
// =============================================================================================================

// The DirectComposition device and a target TOPMOST on the player's window: what is in it is drawn ABOVE the window's
// child windows -- mpv's picture -- by DWM. (A transparent WINDOWED WebView2 cannot blend over a sibling child window: no
// per-pixel alpha between child HWNDs; that is why the page is visual-hosted at all.) False = the windowed fallback.
bool initComposition(HWND hwnd) {
    if (g_dcompTarget) return true;
    if (FAILED(::DCompositionCreateDevice2(nullptr, IID_PPV_ARGS(&g_dcomp))) || !g_dcomp) return false;
    if (FAILED(g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &g_dcompTarget)) || !g_dcompTarget
        || FAILED(g_dcomp->CreateVisual(&g_dcompRoot)) || FAILED(g_dcompTarget->SetRoot(g_dcompRoot.Get()))
        || FAILED(g_dcomp->Commit())) {
        g_dcompRoot.Reset();
        g_dcompTarget.Reset();
        g_dcomp.Reset();
        return false;
    }
    return true;
}

// The CSS pixel -> client pixel factor of the picture's maths: the view's rasterization scale (the monitor's DPI and the
// user's text scale) x its zoom (Ctrl + wheel) = the page's devicePixelRatio.
void updateNativeScale() {
    if (!g_native || !g_controller) return;
    double scale = dpiOfWindow(g_hwnd) / 96.0, zoom = 1.0;
    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(g_controller.As(&c3)) && c3) {
        double r = 0.0;
        if (SUCCEEDED(c3->get_RasterizationScale(&r)) && r > 0.0) scale = r;
    }
    if (FAILED(g_controller->get_ZoomFactor(&zoom)) || !(zoom > 0.0)) zoom = 1.0;
    g_native->setDpiScale(scale * zoom);
}

// ---- the page's cursor ------------------------------------------------------------------------------------------------
// Visual hosting gives the view no window under the mouse: the PAGE'S cursor comes as CursorChanged and is applied here
// (WM_SETCURSOR). nullptr + hidden = CSS cursor:none -- full screen's idle hide. MEASURED by N0 (26/10): Chromium reports
// cursor:none in ~20 ms, but NOT always the way back to the arrow over the same element -- the page's nativeCursor word
// (bridge: kWmTranslatorWindow wParam 5) is the safety net. A TEST INSTANCE never hides the user's real cursor (the arrow
// stays; the change is logged -- N0's incident, 26/10: a full-screen test hid his cursor 0.6 s).
HCURSOR g_pageCursor = nullptr;
bool    g_pageCursorHidden = false;
HCURSOR g_ownedCursors[4] = {};                    // copies of the page's custom cursors (the view may free its own): a ring
unsigned g_ownedNext = 0;

// the real mouse is over the player's CLIENT area (not its title bar or borders: their cursors are Windows')
bool cursorOverPlayer() {
    POINT p{};
    if (!g_hwnd || !::GetCursorPos(&p)) return false;
    HWND h = ::WindowFromPoint(p);
    if (!h || (h != g_hwnd && !::IsChild(g_hwnd, h))) return false;
    RECT rc{};
    ::GetClientRect(g_hwnd, &rc);
    ::ScreenToClient(g_hwnd, &p);
    return ::PtInRect(&rc, p) != FALSE;
}

HCURSOR cursorToSet() {
    // (while the browser process is dead -- a rebuild on its way -- no page owns the cursor: never left hidden)
    if (g_pageCursorHidden && !g_viewsLost) return g_quiet ? ::LoadCursorW(nullptr, IDC_ARROW) : nullptr;
    return (g_pageCursor && !g_viewsLost) ? g_pageCursor : ::LoadCursorW(nullptr, IDC_ARROW);
}

void setPageCursor(HCURSOR c, bool hidden, const char* why) {
    const bool was = g_pageCursorHidden;
    g_pageCursor = c;
    g_pageCursorHidden = hidden;
    // now, not at the next move: the idle hide happens while the mouse is STILL
    if (cursorOverPlayer()) ::SetCursor(cursorToSet());
    if (was != hidden && g_native)
        g_native->diag(std::string("cursor ") + (hidden ? "HIDDEN" : "SHOWN") + " (" + why + ")"
                       + (hidden && g_quiet ? " -- a test instance keeps the arrow" : ""));
}

HRESULT onPageCursorChanged(ICoreWebView2CompositionController* sender) {
    HCURSOR c = nullptr;
    UINT32 sysId = 0;
    if (!sender || FAILED(sender->get_Cursor(&c))) return S_OK;
    sender->get_SystemCursorId(&sysId);
    if (!c) { setPageCursor(nullptr, true, "the page's cursor:none"); return S_OK; }
    if (sysId) { setPageCursor(::LoadCursorW(nullptr, MAKEINTRESOURCEW(sysId)), false, "the page's cursor"); return S_OK; }
    HCURSOR copy = reinterpret_cast<HCURSOR>(::CopyIcon(reinterpret_cast<HICON>(c)));
    setPageCursor(copy ? copy : c, false, "the page's own cursor");
    if (copy) {                                     // the one before the previous goes (never the one on screen)
        if (g_ownedCursors[g_ownedNext]) ::DestroyCursor(g_ownedCursors[g_ownedNext]);
        g_ownedCursors[g_ownedNext] = copy;
        g_ownedNext = (g_ownedNext + 1) % 4;
    }
    return S_OK;
}

// The page's word (nativeCursor {show}, forwarded by the bridge): it LEFT full screen's idle state -- the arrow comes back
// if Chromium did not say so -- or it ENTERED it (the hide, in case Chromium is late).
void pageCursorWord(bool show) {
    if (!g_compController) return;                 // (windowed: the view's own window owns its cursor)
    if (show && g_pageCursorHidden) setPageCursor(::LoadCursorW(nullptr, IDC_ARROW), false, "the page's nativeCursor show: the arrow RESTORED");
    else if (!show && !g_pageCursorHidden) setPageCursor(nullptr, true, "the page's nativeCursor hide");
}

// ---- input forwarded to the page ---------------------------------------------------------------------------------------
bool g_mouseTracked = false;                       // TME_LEAVE armed
// A TEST INSTANCE's option (SUVTITLE_TEST_NO_MOUSELEAVE=1, read in wWinMain): no TME_LEAVE. A move POSTED while the real
// cursor is elsewhere gets an immediate WM_MOUSELEAVE otherwise, and Blink, told the pointer left, reports no cursor (N0).
bool g_noTrackLeave = false;
bool g_viewFocused = false;
std::vector<UINT32> g_pointersIn;                  // pen / touch contacts that began over the view
// (fix round LOW12) the buttons pressed and not released yet, as the page knows them, and where the mouse last was: a
// capture lost in the middle of a drag (Alt+Tab, another program taking the mouse) never brings their button-up -- the page
// is given it (releaseHeldButtons), or its seek bar would drag on with no button down
unsigned g_buttonsHeld = 0;                        // 1 left, 2 right, 4 middle, 8 X1, 16 X2
POINT    g_lastMouse{ 0, 0 };

void focusView() {
    if (g_controller && !g_viewFocused) g_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
}

unsigned buttonBit(UINT msg, WPARAM wParam) {
    switch (msg) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 1;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 2;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 4;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: return GET_XBUTTON_WPARAM(wParam) == XBUTTON2 ? 16u : 8u;
    default: return 0;
    }
}

void releaseHeldButtons() {
    const unsigned held = g_buttonsHeld;
    g_buttonsHeld = 0;
    if (!g_compController || !held) return;
    struct Up { unsigned bit; UINT msg; UINT32 data; };
    const Up ups[] = { { 1, WM_LBUTTONUP, 0 }, { 2, WM_RBUTTONUP, 0 }, { 4, WM_MBUTTONUP, 0 }, { 8, WM_XBUTTONUP, XBUTTON1 },
                       { 16, WM_XBUTTONUP, XBUTTON2 } };
    for (const Up& u : ups)
        if (held & u.bit)
            g_compController->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(u.msg),
                                             COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS_NONE, u.data, g_lastMouse);
    if (g_native) g_native->diag("the mouse capture was lost with a button down: its button-up given to the page");
}

bool forwardMouse(UINT msg, WPARAM wParam, LPARAM lParam) {
    // (fix round LOW5: a leave clears the tracking even while there is no view -- a browser rebuild in between -- or
    // TME_LEAVE would never be armed again and the page never told of a leave for the rest of the session)
    if (msg == WM_MOUSELEAVE) g_mouseTracked = false;
    if (!g_compController) return false;
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    UINT32 data = 0;
    switch (msg) {
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        ::ScreenToClient(g_hwnd, &pt);              // wheel messages carry SCREEN coordinates
        data = static_cast<UINT32>(static_cast<INT32>(GET_WHEEL_DELTA_WPARAM(wParam)));
        break;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
    case WM_XBUTTONDBLCLK:
        data = GET_XBUTTON_WPARAM(wParam);
        break;
    case WM_MOUSEMOVE:
        if (!g_mouseTracked && !g_noTrackLeave) {
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, g_hwnd, 0 };
            ::TrackMouseEvent(&t);
            g_mouseTracked = true;
        }
        break;
    case WM_MOUSELEAVE:
        pt = POINT{ 0, 0 };
        wParam = 0;
        break;
    default: break;
    }
    // (a double-click message IS the second press of the double-click: it takes the mouse and the focus like a press)
    const bool down = msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN
                   || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK || msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK;
    const bool up = msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
    if (msg != WM_MOUSELEAVE) g_lastMouse = pt;
    if (down) {
        g_buttonsHeld |= buttonBit(msg, wParam);
        // a press drags on beyond the window (the seek bar, a slider): the mouse is kept until it is released -- never a
        // TEST INSTANCE's (a posted press must not capture the user's real mouse)
        if (!g_quiet && ::GetCapture() != g_hwnd) ::SetCapture(g_hwnd);
        focusView();
    } else if (up) {
        // (the bit first: releasing the capture sends WM_CAPTURECHANGED at once, and this button is no longer held)
        g_buttonsHeld &= ~buttonBit(msg, wParam);
        if (!g_buttonsHeld && ::GetCapture() == g_hwnd) ::ReleaseCapture();
    }
    g_compController->SendMouseInput(static_cast<COREWEBVIEW2_MOUSE_EVENT_KIND>(msg),
                                     static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GET_KEYSTATE_WPARAM(wParam)), data, pt);
    return true;
}

// Pen and touch (WM_POINTER*; the mouse sends none: EnableMouseInPointer is not called) -- N0's forwardPointer.
bool forwardPointer(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (!g_compController || !g_environment) return false;
    const UINT32 id = GET_POINTERID_WPARAM(wParam);
    POINT pt{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    ::ScreenToClient(g_hwnd, &pt);
    RECT rc{};
    ::GetClientRect(g_hwnd, &rc);
    const bool inside = ::PtInRect(&rc, pt) != FALSE;
    auto it = std::find(g_pointersIn.begin(), g_pointersIn.end(), id);
    const bool tracked = it != g_pointersIn.end();
    if (!inside && !tracked) return false;
    if (msg == WM_POINTERDOWN && inside && !tracked) g_pointersIn.push_back(id);
    if ((msg == WM_POINTERUP || msg == WM_POINTERLEAVE) && tracked) g_pointersIn.erase(it);
    ComPtr<ICoreWebView2Environment3> env3;
    ComPtr<ICoreWebView2PointerInfo> pi;
    if (FAILED(g_environment.As(&env3)) || !env3 || FAILED(env3->CreateCoreWebView2PointerInfo(&pi)) || !pi) return false;
    POINTER_INPUT_TYPE type = PT_POINTER;
    ::GetPointerType(id, &type);
    POINTER_INFO info{};
    POINTER_PEN_INFO pen{};
    POINTER_TOUCH_INFO touch{};
    if (type == PT_PEN) { if (!::GetPointerPenInfo(id, &pen)) return false; info = pen.pointerInfo; }
    else if (type == PT_TOUCH) { if (!::GetPointerTouchInfo(id, &touch)) return false; info = touch.pointerInfo; }
    else if (!::GetPointerInfo(id, &info)) return false;
    RECT devRect{}, dispRect{};
    ::GetPointerDeviceRects(info.sourceDevice, &devRect, &dispRect);
    POINT origin{ 0, 0 };
    ::ClientToScreen(g_hwnd, &origin);
    const double rx = double(devRect.right - devRect.left) / double(std::max<LONG>(1, dispRect.right - dispRect.left));
    const double ry = double(devRect.bottom - devRect.top) / double(std::max<LONG>(1, dispRect.bottom - dispRect.top));
    auto toClient = [](POINT p) { ::ScreenToClient(g_hwnd, &p); return p; };
    auto toClientHimetric = [&](POINT p) {
        p.x -= static_cast<LONG>(std::lround(double(origin.x - dispRect.left) * rx));
        p.y -= static_cast<LONG>(std::lround(double(origin.y - dispRect.top) * ry));
        return p;
    };
    pi->put_PointerKind(static_cast<DWORD>(info.pointerType));
    pi->put_PointerId(info.pointerId);
    pi->put_FrameId(info.frameId);
    pi->put_PointerFlags(static_cast<UINT32>(info.pointerFlags));
    pi->put_PointerDeviceRect(devRect);
    pi->put_DisplayRect(dispRect);
    pi->put_PixelLocation(toClient(info.ptPixelLocation));
    pi->put_PixelLocationRaw(toClient(info.ptPixelLocationRaw));
    pi->put_HimetricLocation(toClientHimetric(info.ptHimetricLocation));
    pi->put_HimetricLocationRaw(toClientHimetric(info.ptHimetricLocationRaw));
    pi->put_Time(info.dwTime);
    pi->put_HistoryCount(info.historyCount);
    pi->put_InputData(info.InputData);
    pi->put_KeyStates(info.dwKeyStates);
    pi->put_PerformanceCount(info.PerformanceCount);
    pi->put_ButtonChangeKind(static_cast<INT32>(info.ButtonChangeType));
    if (type == PT_PEN) {
        pi->put_PenFlags(pen.penFlags);
        pi->put_PenMask(pen.penMask);
        pi->put_PenPressure(pen.pressure);
        pi->put_PenRotation(pen.rotation);
        pi->put_PenTiltX(pen.tiltX);
        pi->put_PenTiltY(pen.tiltY);
    } else if (type == PT_TOUCH) {
        auto rcToClient = [&](RECT r) {
            const POINT a = toClient(POINT{ r.left, r.top }), b = toClient(POINT{ r.right, r.bottom });
            return RECT{ a.x, a.y, b.x, b.y };
        };
        pi->put_TouchFlags(touch.touchFlags);
        pi->put_TouchMask(touch.touchMask);
        pi->put_TouchContact(rcToClient(touch.rcContact));
        pi->put_TouchContactRaw(rcToClient(touch.rcContactRaw));
        pi->put_TouchOrientation(touch.orientation);
        pi->put_TouchPressure(touch.pressure);
    }
    if (msg == WM_POINTERDOWN) focusView();
    g_compController->SendPointerInput(static_cast<COREWEBVIEW2_POINTER_EVENT_KIND>(msg), pi.Get());
    return true;
}

// ---- file drops ----------------------------------------------------------------------------------------------------------
// The user, 11/10: "qu'on puisse drag & drop les fichiers audio/video dans le lecteur". A visual-hosted view has no window
// to register a drop target on: the HOST's window takes the drag (OLE) and hands every step to the composition controller,
// so the PAGE'S drop zones see the drop exactly as before -- its File objects (pin D1: the paths are WebView2's, never the
// page's JSON). The shell's drop helper keeps the dragged picture under the cursor.
class DropForwarder final : public IDropTarget {
public:
    explicit DropForwarder(HWND h) : hwnd_(h) {
        ::CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper_));
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *ppv = static_cast<IDropTarget*>(this); AddRef(); return S_OK; }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(::InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = ::InterlockedDecrement(&ref_);
        if (!r) delete this;
        return static_cast<ULONG>(r);
    }
    // (fix round LOW11: the drag-image helper is told the PAGE'S answer -- after the view -- not the effects the source
    // allows: its badge shows what the drop will really do)
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL at, DWORD* effect) override {
        POINT sp{ at.x, at.y };
        ComPtr<ICoreWebView2CompositionController3> c3 = view();
        const HRESULT hr = c3 ? c3->DragEnter(data, keys, client(at), effect) : S_OK;
        if (!c3) *effect = DROPEFFECT_NONE;
        if (helper_) helper_->DragEnter(hwnd_, data, &sp, *effect);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL at, DWORD* effect) override {
        POINT sp{ at.x, at.y };
        ComPtr<ICoreWebView2CompositionController3> c3 = view();
        const HRESULT hr = c3 ? c3->DragOver(keys, client(at), effect) : S_OK;
        if (!c3) *effect = DROPEFFECT_NONE;
        if (helper_) helper_->DragOver(&sp, *effect);
        return hr;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (helper_) helper_->DragLeave();
        ComPtr<ICoreWebView2CompositionController3> c3 = view();
        return c3 ? c3->DragLeave() : S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL at, DWORD* effect) override {
        POINT sp{ at.x, at.y };
        ComPtr<ICoreWebView2CompositionController3> c3 = view();
        const HRESULT hr = c3 ? c3->Drop(data, keys, client(at), effect) : S_OK;
        if (!c3) *effect = DROPEFFECT_NONE;
        if (helper_) helper_->Drop(data, &sp, *effect);
        if (c3 && g_native) g_native->diag("a file drop handed to the page (effect " + std::to_string(*effect) + ")");
        return hr;
    }
private:
    ~DropForwarder() = default;
    static ComPtr<ICoreWebView2CompositionController3> view() {
        ComPtr<ICoreWebView2CompositionController3> c3;
        if (g_compController) g_compController.As(&c3);
        return c3;
    }
    POINT client(POINTL at) const {              // the view's coordinates = the window's client (its bounds start at 0,0)
        POINT p{ at.x, at.y };
        ::ScreenToClient(hwnd_, &p);
        return p;
    }
    LONG ref_ = 1;
    HWND hwnd_;
    ComPtr<IDropTargetHelper> helper_;
};
DropForwarder* g_drops = nullptr;                  // registered once (RegisterDragDrop holds its own reference)

// A TEST INSTANCE's drop (WM_COPYDATA kCopyTestDrop -> kWmTestDrop): the file as the shell's data object -- what Explorer
// hands over in a real drag -- through the SAME forwarder, on the middle of the picture (the player's drop zone, not the
// queue's), else of the window. The real OLE loop needs the real mouse, which no test may move.
void testDrop() {
    const std::wstring path = g_testDropPath;
    g_testDropPath.clear();
    if (!g_quiet || !g_drops || path.empty()) return;
    ComPtr<IShellItem> item;
    ComPtr<IDataObject> data;
    if (FAILED(::SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item
        || FAILED(item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data))) || !data) {
        if (g_native) g_native->diag("test drop: no data object for that path");
        return;
    }
    RECT rc{};
    HWND pic = g_native ? g_native->videoWindow() : nullptr;
    if (pic && ::IsWindowVisible(pic)) ::GetWindowRect(pic, &rc);
    else { ::GetClientRect(g_hwnd, &rc); ::MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2); }
    const POINTL at{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_LINK;
    g_drops->DragEnter(data.Get(), MK_LBUTTON, at, &eff);
    eff = DROPEFFECT_COPY | DROPEFFECT_LINK;
    g_drops->DragOver(MK_LBUTTON, at, &eff);
    eff = DROPEFFECT_COPY | DROPEFFECT_LINK;
    g_drops->Drop(data.Get(), 0, at, &eff);
}

// UI Automation: a visual-hosted view's tree is handed out by the host's window (screen readers, the narrator).
LRESULT automationObject(HWND hwnd, WPARAM wParam, LPARAM lParam) {
    if (!g_compController || static_cast<long>(lParam) != static_cast<long>(UiaRootObjectId)) return -1;
    ComPtr<ICoreWebView2CompositionController2> c2;
    ComPtr<IUnknown> provider;
    ComPtr<IRawElementProviderSimple> simple;
    if (FAILED(g_compController.As(&c2)) || !c2 || FAILED(c2->get_AutomationProvider(&provider)) || !provider
        || FAILED(provider.As(&simple)) || !simple) return -1;
    return ::UiaReturnRawElementProvider(hwnd, wParam, lParam, simple.Get());
}

// A NEW composition controller (at the start, and after a rebuild): transparent, in a FRESH visual of the topmost target,
// its cursor, focus and scale watched, the window's drop target registered (once).
void setupVisualHosting() {
    if (!g_compController || !g_controller || !g_dcomp || !g_dcompRoot) return;
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(g_controller.As(&c2)) && c2) c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{ 0, 0, 0, 0 });
    if (g_dcompView) { g_dcompRoot->RemoveVisual(g_dcompView.Get()); g_dcompView.Reset(); }
    if (SUCCEEDED(g_dcomp->CreateVisual(&g_dcompView)) && g_dcompView) {
        g_dcompRoot->AddVisual(g_dcompView.Get(), TRUE, nullptr);
        g_compController->put_RootVisualTarget(g_dcompView.Get());
    }
    g_dcomp->Commit();
    EventRegistrationToken t{};
    g_compController->add_CursorChanged(
        Callback<ICoreWebView2CursorChangedEventHandler>([](ICoreWebView2CompositionController* s, IUnknown*) -> HRESULT {
            return onPageCursorChanged(s);
        }).Get(), &t);
    g_controller->add_GotFocus(Callback<ICoreWebView2FocusChangedEventHandler>([](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
        g_viewFocused = true;
        return S_OK;
    }).Get(), &t);
    g_controller->add_LostFocus(Callback<ICoreWebView2FocusChangedEventHandler>([](ICoreWebView2Controller*, IUnknown*) -> HRESULT {
        g_viewFocused = false;
        return S_OK;
    }).Get(), &t);
    ComPtr<ICoreWebView2Controller3> c3;
    if (SUCCEEDED(g_controller.As(&c3)) && c3)
        c3->add_RasterizationScaleChanged(Callback<ICoreWebView2RasterizationScaleChangedEventHandler>(
            [](ICoreWebView2Controller*, IUnknown*) -> HRESULT { updateNativeScale(); return S_OK; }).Get(), &t);
    g_controller->add_ZoomFactorChanged(Callback<ICoreWebView2ZoomFactorChangedEventHandler>(
        [](ICoreWebView2Controller*, IUnknown*) -> HRESULT { updateNativeScale(); return S_OK; }).Get(), &t);
    g_viewFocused = false;
    g_pageCursor = nullptr;
    g_pageCursorHidden = false;
    if (!g_drops) {
        g_drops = new DropForwarder(g_hwnd);
        if (FAILED(::RegisterDragDrop(g_hwnd, g_drops))) { g_drops->Release(); g_drops = nullptr; }
    }
    updateNativeScale();
}

std::wstring nativeLogPath() {
    wchar_t la[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    const std::wstring dir = std::wstring(la) + L"\\SuVtitle";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\native.log";
}

// mpv, ONCE per app, after the window exists (posted by WM_CREATE): libmpv-2.dll from <exe>\mpv\ (the bundle) or next to
// the exe, its disabled child in this window, idle. A failure is a sentence for the page (nativeInfo), never a dialog.
void startNative() {
    if (g_nativeTried || !g_hwnd) return;
    g_nativeTried = true;
    g_native = std::make_unique<suvmpv::MpvPlayer>();
    suvmpv::MpvOptions o;
    o.silent = g_quiet;                            // a TEST INSTANCE never makes a sound
    o.logPath = nativeLogPath();
    g_native->setOptions(o);
    std::string why;
    const bool dll = g_native->load(suvmpv::MpvPlayer::defaultDllDir(), why);   // (native.log is open from here on)
    g_native->diag(g_dllLocked ? "DLL search locked: the exe's folder, System32 and mpv\\ -- never the current directory"
                               : "DLL search NOT locked (SetDefaultDllDirectories refused): the default order stays");
    if (!dll) g_nativeWhy = why;
    else if (!g_native->attach(g_hwnd)) g_nativeWhy = g_native->why();
    else g_nativeWhy.clear();
    if (g_native->available()) {
        updateNativeScale();
        if (g_fullscreen) g_native->setFullscreenHint(true);
    }
    attachNativeToBridge();
}

// The bridge gets mpv once the view is attached AND mpv's start was tried: the player -- or nullptr and why (the page
// then plays everything with its <video>). Again after a rebuild (Bridge::attachNative is callable again).
void attachNativeToBridge() {
    if (!g_bridge || !g_webview || !g_nativeTried) return;
    if (!g_compController)
        g_bridge->attachNative(nullptr, "the page is not hosted over the video (this WebView2 runtime or DirectComposition "
                                        "refused visual hosting)");
    else if (g_native && g_native->available())
        g_bridge->attachNative(g_native.get());
    else
        g_bridge->attachNative(nullptr, g_nativeWhy.empty() ? std::string("the native video engine did not start") : g_nativeWhy);
}

// The app's end: mpv terminated while its window is still a child of ours (MpvPlayer::shutdown answers mpv's window
// meanwhile) -- AFTER the bridge let it go (its sink captures the Bridge: NATIVE_CONTRACT.md section 6).
void endNative() {
    if (g_native) g_native->shutdown();
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
    const unsigned egen = g_envGen;   // (fix round: this creation's generation -- a newer rebuild makes it stale)
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, userDataFolder, opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd, userDataFolder, withSwitches, egen](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (egen != g_envGen) return S_OK;   // stale: a newer rebuild owns the views now (its env is let go)
                if ((FAILED(r) || !env) && withSwitches) {
                    createWebViewEnv(hwnd, userDataFolder, false);   // once more, without the switches
                    return S_OK;
                }
                // SUVTITLE LIVE fix round (RVB-4): a REBUILD that fails (the profile still locked by the dying browser,
                // the runtime being repaired) is tried again later -- the engine and the journal live on; never the
                // startup's MessageBox-and-quit in the middle of a meeting.
                if ((FAILED(r) || !env) && g_rebuilding) { rebuildFailed(); return S_OK; }
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
                // 1.1.0 (N1): the player's view is VISUAL-HOSTED -- a composition controller (created below), its page
                // drawn over mpv's picture -- or, when this runtime or DirectComposition cannot, 1.0's windowed view. Either
                // way THIS handler sets the view up; `comp` = the composition controller (null: the windowed view).
                const auto onController =
                        [hwnd, egen](HRESULT r2, ICoreWebView2Controller* controller,
                                     ICoreWebView2CompositionController* comp) -> HRESULT {
                            if (egen != g_envGen) {                  // stale (fix round): never ours -- closed, nothing touched
                                if (controller) controller->Close();
                                return S_OK;
                            }
                            if ((FAILED(r2) || !controller) && g_rebuilding) {   // (fix round, RVB-4: tried again)
                                rebuildFailed();
                                return S_OK;
                            }
                            if (FAILED(r2) || !controller) {
                                ::MessageBoxW(hwnd, L"SuVtitle could not create its WebView2 view.",
                                              L"SuVtitle", MB_ICONERROR | MB_OK);
                                ::DestroyWindow(hwnd);
                                return r2;
                            }
                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);
                            if (!g_webview && g_rebuilding) { g_controller->Close(); g_controller.Reset(); rebuildFailed(); return S_OK; }
                            if (!g_webview) { ::DestroyWindow(hwnd); return E_FAIL; }
                            // (fix round: a rebuild has its view back -- the windows' "being restored" notice ends)
                            if (g_rebuilding) { g_rebuilding = false; g_rebuildFails = 0; g_viewsLost = false; }
                            // 1.1.0 (N1): the page over mpv's picture -- transparent, in a fresh visual of the topmost
                            // target, its cursor / focus / scale watched, the window's drops forwarded to it
                            g_compController = comp;
                            setupVisualHosting();
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
                            // SUVTITLE LIVE: a renderer that dies (or hangs) is reloaded by itself (ProcessFailed)
                            watchProcessFailed(g_webview.Get(), /*live=*/false);
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
                                attachNativeToBridge();   // (N1: mpv -- or why not -- once its start was tried)
                            }
                            // WG: started by --play (Explorer's double-click): the files, now that the view can take them
                            if (g_playRequested || !g_pendingPlay.empty()) ::PostMessageW(hwnd, kWmPlayPending, 0, 0);
                            resizeView();
                            // SUVTITLE LIVE: a player window that is not shown (--live) has an invisible view (throttled),
                            // and the Live window asked for meanwhile -- --live, a forwarded --live -- opens now, after
                            // the player's view: the engine started at its attach, the environment is there.
                            if (!::IsWindowVisible(hwnd)) g_controller->put_IsVisible(FALSE);
                            if (g_liveRequested)
                                ::PostMessageW(hwnd, patchbus::Bridge::kWmTranslatorWindow, patchbus::Bridge::kWinOpenLive, 0);
                            // SUVTITLE LIVE fix round (RVB-3): after a REBUILD the Live window gets its VIEW back -- only
                            // that: never shown, restored nor activated (a recovery is no user's gesture)
                            if (g_liveViewOwed) { g_liveViewOwed = false; g_liveViewTries = 0; createLiveView(/*user=*/false); }
                            ::InvalidateRect(hwnd, nullptr, TRUE);

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
                        };
                // (N1) 1.0's WINDOWED view only when visual hosting is UNSUPPORTED -- no DirectComposition, a runtime without
                // ICoreWebView2Environment3, or an answer saying so: the page then plays everything with its <video>
                // (attachNativeToBridge says why). Any OTHER failure is the view's own (fix round LOW6: a transient one, during a
                // rebuild, switched the whole session to windowed and detached mpv): onController's failure path -- a rebuild
                // tries again later. The windowed creation's own refusal is checked too (a view never left uncreated).
                const auto unsupported = [](HRESULT r) {
                    return r == E_NOINTERFACE || r == E_NOTIMPL || r == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
                };
                const auto windowed = [onController, envKeep = ComPtr<ICoreWebView2Environment>(env), hwnd]() {
                    const HRESULT w = envKeep->CreateCoreWebView2Controller(
                        hwnd,
                        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [onController](HRESULT r4, ICoreWebView2Controller* wc) -> HRESULT {
                                return onController(r4, wc, nullptr);
                            }).Get());
                    if (FAILED(w)) onController(w, nullptr, nullptr);
                };
                ComPtr<ICoreWebView2Environment3> env3;
                if (!g_dcompRoot || FAILED(env->QueryInterface(IID_PPV_ARGS(&env3))) || !env3) { windowed(); return S_OK; }
                const HRESULT made = env3->CreateCoreWebView2CompositionController(
                    hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                        [onController, windowed, unsupported, egen](HRESULT r3, ICoreWebView2CompositionController* comp) -> HRESULT {
                            ComPtr<ICoreWebView2Controller> c;
                            if (SUCCEEDED(r3) && comp) comp->QueryInterface(IID_PPV_ARGS(&c));
                            if (!c && egen == g_envGen && unsupported(r3)) { windowed(); return S_OK; }
                            return onController(r3, c.Get(), c ? comp : nullptr);
                        }).Get());
                if (FAILED(made)) {
                    if (unsupported(made)) windowed();
                    else onController(made, nullptr, nullptr);
                }
                return S_OK;
            }).Get());

    if (FAILED(hr) && withSwitches) { createWebViewEnv(hwnd, userDataFolder, false); return; }
    if (FAILED(hr) && g_rebuilding) { rebuildFailed(); return; }   // (fix round, RVB-4: a rebuild never quits the app)
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
        initComposition(hwnd);       // (N1: the page's TOPMOST DirectComposition target -- before its view is asked for)
        createWebView(hwnd);
        ::SetTimer(hwnd, kUiTickTimer, 1000, nullptr);   // drives Bridge::uiTick
        ::PostMessageW(hwnd, kWmNativeStart, 0, 0);      // (N1: mpv, once the window is up)
        return 0;

    // ---- 1.1.0 THE NATIVE VIDEO ENGINE (N1): what visual hosting moves to the host --------------------------------------
    case kWmNativeStart:
        startNative();
        return 0;
    case kWmTestDrop:
        testDrop();
        return 0;
    // the mouse: forwarded to the page (the view has no window under it); the picture's window is disabled, so every real
    // mouse message over it comes HERE
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: case WM_MOUSELEAVE:
        if (forwardMouse(msg, wParam, lParam))
            return (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK) ? TRUE : 0;
        break;
    case WM_POINTERDOWN: case WM_POINTERUP: case WM_POINTERUPDATE: case WM_POINTERENTER: case WM_POINTERLEAVE:
        if (forwardPointer(msg, wParam, lParam)) return 0;
        break;
    case WM_POINTERACTIVATE:
        forwardPointer(msg, wParam, lParam);
        break;                                       // (DefWindowProc answers: a test instance's window never activates)
    // (fix round LOW12) the mouse capture taken away in the middle of a press (Alt+Tab, another program): the page gets
    // the button-ups it would never see -- a seek bar dragging with no button down otherwise
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lParam) != hwnd) releaseHeldButtons();
        break;
    case WM_SETCURSOR:
        if (g_compController && LOWORD(lParam) == HTCLIENT) { ::SetCursor(cursorToSet()); return TRUE; }
        break;
    // the keyboard: the page's (the view's own input window, once it has the focus)
    case WM_SETFOCUS:
        focusView();
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (g_compController) focusView();           // (a key that reached the HOST: the page did not have the focus yet)
        break;
    case WM_MOVE:
    case WM_MOVING:
        if (g_controller) g_controller->NotifyParentWindowPositionChanged();   // (the page's <select> popups)
        break;
    case WM_GETOBJECT: {
        const LRESULT r = automationObject(hwnd, wParam, lParam);
        if (r != -1) return r;
        break;
    }

    case WM_TIMER:
        if (wParam == kPlayBatchTimer || wParam == kPlayRetryTimer) {   // WG: the --play batch is closed / retried
            ::KillTimer(hwnd, wParam);
            ::PostMessageW(hwnd, kWmPlayPending, 0, 0);
            return 0;
        }
        // SUVTITLE LIVE fix round: the last window closed while the engine was live -- it has FINISHED (STATE live 0), or
        // stopped, or the wait is over: the app ends now (WM_DESTROY: the bridge's shutdown tells the engine to QUIT)
        if (wParam == kQuitWaitTimer) {
            if (!g_quitting) { ::KillTimer(hwnd, wParam); return 0; }
            if (!g_bridge || !g_bridge->liveDraining() || ::GetTickCount64() >= g_quitDeadline) {
                ::KillTimer(hwnd, wParam);
                g_quitting = false;
                ::DestroyWindow(hwnd);
            }
            return 0;
        }
        // ...a rebuild owed (the limiter) or tried again (it failed), a reload owed, a Live view to make again (RVB-4)
        if (wParam == kRebuildTimer) {
            ::KillTimer(hwnd, wParam);
            if (g_rebuildRetry) { g_rebuildRetry = false; g_rebuilding = true; createWebView(hwnd); }
            else rebuildViews(g_envGen);
            return 0;
        }
        if (wParam == kReloadTimer0 || wParam == kReloadTimer1) {
            ::KillTimer(hwnd, wParam);
            const int v = wParam == kReloadTimer1 ? 1 : 0;
            g_reloadOwed[v] = false;
            reloadView(v, v ? g_liveWebview.Get() : g_webview.Get());
            return 0;
        }
        if (wParam == kLiveViewTimer) {
            ::KillTimer(hwnd, wParam);
            createLiveView(/*user=*/false);
            return 0;
        }
        if (wParam == kUiTickTimer && g_bridge) g_bridge->uiTick();
        return 0;

    // SUVTITLE LIVE fix round (RVB-4): while the browser is gone (its views with it), the window says so in words
    case WM_PAINT:
        if (paintNoView(hwnd)) return 0;
        break;

    case WM_SIZE:
        resizeView();
        if (g_native) g_native->parentResized();     // (N1: the picture's place, applied again -- full screen's at once)
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
        updateNativeScale();     // (N1: the page's CSS pixels -> the picture's pixels; RasterizationScaleChanged says it too)
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
        // SUVTITLE LIVE (1.1.0): the Live window -- opened or brought up (the player's Live button, --live, a forwarded
        // --live), and its own always-on-top (its page's pin)
        else if (wParam == patchbus::Bridge::kWinOpenLive)    openLiveWindow();
        else if (wParam == patchbus::Bridge::kWinLiveTopmost) setLiveTopmost(lParam != 0);
        // 1.1.0 (N1): the page's nativeCursor word -- it left full screen's idle state (1: the arrow back if Chromium did
        // not bring it) or entered it (0)
        else if (wParam == patchbus::Bridge::kWinNativeCursor) pageCursorWord(lParam != 0);
        return 0;

    // SUVTITLE LIVE: the browser process died (ProcessFailed, posted once per view): both views rebuilt, once.
    case kWmRebuildViews:
        rebuildViews(static_cast<unsigned>(wParam));   // (the generation of the views that died: an older one is ignored)
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
        // 1.1.0 (N1): a TEST INSTANCE's drop -- a path (UTF-16, no terminator needed) dropped through the window's drop
        // target, off this SendMessage. Anything else, and any other instance, refuses it.
        if (cds && cds->dwData == kCopyTestDrop) {
            if (!g_quiet || !cds->lpData || cds->cbData < sizeof(wchar_t) || cds->cbData > 32768 * sizeof(wchar_t)
                || (cds->cbData % sizeof(wchar_t)) != 0) return FALSE;
            g_testDropPath.assign(static_cast<const wchar_t*>(cds->lpData), cds->cbData / sizeof(wchar_t));
            while (!g_testDropPath.empty() && g_testDropPath.back() == L'\0') g_testDropPath.pop_back();
            ::PostMessageW(hwnd, kWmTestDrop, 0, 0);
            return TRUE;
        }
        // SUVTITLE LIVE (1.1.0): ANOTHER SuVtitle was started with --live (the "SuVtitle Live" shortcut): the Live window
        // opens HERE -- off this SendMessage (posted), like --play's paths. It carries nothing: an empty payload only.
        // (fix round: an ACCEPTED copy while the app waits for the live drain before ending -- it is wanted again: it
        // stays, at once, so no deadline can fall between this copy and the window it brings back; a refused copy changes
        // nothing -- it must not leave an app with no window alive)
        if (cds && cds->dwData == pblaunch::kCopyLive) {
            if (!pblaunch::isLiveCopy(cds->dwData, cds->lpData, cds->cbData)) return FALSE;
            cancelQuit();
            ::PostMessageW(hwnd, patchbus::Bridge::kWmTranslatorWindow, patchbus::Bridge::kWinOpenLive, 0);
            return TRUE;
        }
        std::vector<std::wstring> paths;
        if (!cds || cds->dwData != pblaunch::kCopyPlay || !pblaunch::unpackPlay(cds->lpData, cds->cbData, paths)) return FALSE;
        cancelQuit();
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
    // SUVTITLE LIVE (1.1.0): ...when it is the LAST window -- with the Live window open, the player only HIDES (its
    // page pauses; a second launch, --play or the Live window's way back brings it up again).
    case WM_CLOSE:
        if (g_fullscreen) setFullscreen(false);
        saveWinPos();
        if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) { hidePlayerWindow(); return 0; }
        quitWhenDrained();   // (fix round: the last window -- a live session still being finished is waited for, hidden)
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
        // SUVTITLE LIVE (1.1.0): the Live window goes FIRST (AudioPB 1.1.0's order): its view must stop receiving posts
        // -- and stop holding a handler that captures the Bridge -- before the Bridge shuts down.
        if (g_liveHwnd) { saveLivePos(); ::DestroyWindow(g_liveHwnd); }
        if (g_bridge) g_bridge->shutdown();   // the player saved, the engine stopped (QUIT, then forced)
        // 1.1.0 (N1): mpv ends now -- AFTER the bridge let it go (its sink captures the Bridge), BEFORE this window's
        // children go (mpv's own window is one of them, on mpv's thread: MpvPlayer::shutdown answers it meanwhile)
        endNative();
        if (g_drops) { ::RevokeDragDrop(hwnd); g_drops->Release(); g_drops = nullptr; }
        if (g_controller) g_controller->Close();
        g_compController.Reset();
        // (fix round LOW6: the view's visual taken out of the topmost target, committed, before the device goes)
        if (g_dcompRoot && g_dcompView) g_dcompRoot->RemoveVisual(g_dcompView.Get());
        if (g_dcomp) g_dcomp->Commit();
        g_dcompView.Reset();
        g_dcompRoot.Reset();
        g_dcompTarget.Reset();
        g_dcomp.Reset();
        for (HCURSOR& c : g_ownedCursors) if (c) { ::DestroyCursor(c); c = nullptr; }
        g_hwnd = nullptr;
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// =============================================================================================================
// SUVTITLE LIVE (1.1.0, 2026-10-25): THE LIVE WINDOW -- AudioPB Translator 1.1.0's translator window (50f79d6,
// ui/main.cpp: TrWndProc, openTranslatorWindow, saveTranslatorWinPos, setTranslatorTopmost), with SuVtitle's rules:
// app.txt LIVEPOS instead of a line in the helper's translator.txt (S11: one owner per file), the REAL topmost read back
// (RV4-7), the title bar brought back onto a monitor, a test instance's own class and no activation, the last-window rule.
// =============================================================================================================

void resizeLiveView() {
    if (!g_liveController || !g_liveHwnd) return;
    RECT rc; ::GetClientRect(g_liveHwnd, &rc);
    g_liveController->put_Bounds(rc);
}

// app.txt LIVEPOS (AppConfig.h): the RESTORED rectangle + topmost + maximized -- WINPOS's fields. A hidden Live window is
// where it was when it was last shown (its placement then says nothing of a maximized state): the last visible save
// stands. The same "an empty read is not an empty file" rule as saveWinPos.
void saveLivePos() {
    if (!g_liveHwnd || !::IsWindowVisible(g_liveHwnd)) return;
    WINDOWPLACEMENT wp{}; wp.length = sizeof(wp);
    if (!::GetWindowPlacement(g_liveHwnd, &wp)) return;
    const RECT& n = wp.rcNormalPosition;
    pbapp::WinPos p;
    p.x = n.left; p.y = n.top; p.w = n.right - n.left; p.h = n.bottom - n.top;
    p.topmost = g_liveTopmost;
    p.maximized = wp.showCmd == SW_SHOWMAXIMIZED || (wp.showCmd == SW_SHOWMINIMIZED && (wp.flags & WPF_RESTORETOMAXIMIZED));
    const std::wstring f = appTxtPath();
    if (f.empty()) return;
    std::string old;
    if (!pbtrf::readFileAll(f, old, 1ULL << 20) && ::GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const std::string body = pbapp::packLivePos(old, p);
    if (body != old) pbtrf::writeFileAvSafe(f, body);   // temp + rename with the antivirus retry (S11)
}

// The Live window's always-on-top (its page's pin): what Windows MADE of the request is kept and told -- every time, a
// refusal included (RV4-7: the pin flipped when it was clicked and must come back to the truth).
void setLiveTopmost(bool on) {
    if (!g_liveHwnd) { g_liveTopmost = on; return; }
    ::SetWindowPos(g_liveHwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    g_liveTopmost = realTopmost(g_liveHwnd);
    saveLivePos();
    if (g_bridge) g_bridge->liveTopmostChanged(g_liveTopmost);
}

// Closing the Live window = listening stops (D-L2: here the window IS the switch -- in AudioPB the card was), and the
// window HIDES: its view invisible (throttled; the conversation still arrives and is all there when it comes back). If
// the player is not shown either, this was the app's LAST window: it quits (WM_DESTROY of the player's window takes the
// Live window down first, then the bridge) -- fix round: once the engine has FINISHED what it heard (quitWhenDrained).
void closeLiveWindow() {
    if (!g_liveHwnd) return;
    saveLivePos();
    if (g_bridge) g_bridge->liveWindowClosed();
    if (g_liveController) g_liveController->put_IsVisible(FALSE);
    ::ShowWindow(g_liveHwnd, SW_HIDE);
    if (!g_hwnd || !::IsWindowVisible(g_hwnd)) quitWhenDrained();
}

// SUVTITLE LIVE fix round (the ADDENDUM FIX ROUND, RVA-1): THE APP'S LAST WINDOW CLOSED. Everything is hidden at once --
// the user asked to quit -- and listening stops (LIVE 0); but LIVE 0 means "stop listening, then FINISH": the engine is
// still turning what it had heard into lines (STATE live stays 1 until they are done) and the journal is waiting for
// them. QUIT does not drain, so the app ends only once the engine says live 0 -- or kQuitDrainMs later, or at once if it
// was not live. A relaunch meanwhile (WM_COPYDATA, bringUp, openLiveWindow) cancels the end: the app is wanted again.
void quitWhenDrained() {
    if (!g_hwnd) { ::PostQuitMessage(0); return; }
    if (::IsWindowVisible(g_hwnd)) hidePlayerWindow();
    if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) {
        saveLivePos();
        if (g_liveController) g_liveController->put_IsVisible(FALSE);
        ::ShowWindow(g_liveHwnd, SW_HIDE);
    }
    if (g_bridge) g_bridge->liveWindowClosed();          // LIVE 0, if it still listens (the drain begins)
    if (!g_bridge || !g_bridge->liveDraining()) { ::DestroyWindow(g_hwnd); return; }
    g_quitting = true;
    g_quitDeadline = ::GetTickCount64() + kQuitDrainMs;
    ::SetTimer(g_hwnd, kQuitWaitTimer, 200, nullptr);
}
void cancelQuit() {
    if (!g_quitting) return;
    g_quitting = false;
    if (g_hwnd) ::KillTimer(g_hwnd, kQuitWaitTimer);
}

// RVB-4: the browser process is gone, and with it the views -- the windows say so in words (a white window with no
// explanation in the middle of a meeting reads as a crash), until the rebuild brings the views back. False = not now.
bool paintNoView(HWND hwnd) {
    if (!g_viewsLost) return false;
    PAINTSTRUCT ps{};
    HDC dc = ::BeginPaint(hwnd, &ps);
    if (!dc) return true;
    RECT rc{};
    ::GetClientRect(hwnd, &rc);
    HBRUSH bg = ::CreateSolidBrush(RGB(0x14, 0x16, 0x1a));
    ::FillRect(dc, &rc, bg);
    ::DeleteObject(bg);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, RGB(0xc8, 0xcc, 0xd4));
    HGDIOBJ old = ::SelectObject(dc, ::GetStockObject(DEFAULT_GUI_FONT));
    const wchar_t* text = L"The window's display stopped (its WebView2 browser process ended).\n"
                          L"SuVtitle is bringing it back. Listening and the session file go on meanwhile.";
    RECT box{ rc.left + 16, rc.top, rc.right - 16, rc.bottom };
    ::DrawTextW(dc, text, -1, &box, DT_CENTER | DT_WORDBREAK | DT_CALCRECT);
    const LONG h = box.bottom - box.top;
    RECT at{ rc.left + 16, rc.top + (rc.bottom - rc.top - h) / 2, rc.right - 16, rc.bottom };
    ::DrawTextW(dc, text, -1, &at, DT_CENTER | DT_WORDBREAK);
    ::SelectObject(dc, old);
    ::EndPaint(hwnd, &ps);
    return true;
}

LRESULT CALLBACK LiveWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SIZE:
        resizeLiveView();
        // a maximise / restore never goes through WM_EXITSIZEMOVE: remembered here
        if (wParam == SIZE_MAXIMIZED || wParam == SIZE_RESTORED) {
            static WPARAM lastLive = SIZE_RESTORED;
            if (wParam != lastLive) { lastLive = wParam; saveLivePos(); }
        }
        return 0;
    case WM_GETMINMAXINFO: {
        // compact (it sits beside a call), never so small its plate and its last lines have no room
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = dpiOfWindow(hwnd);
        mmi->ptMinTrackSize.x = (LONG)(360 * dpi / 96);
        mmi->ptMinTrackSize.y = (LONG)(240 * dpi / 96);
        return 0;
    }
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lParam);
        ::SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        resizeLiveView();
        return 0;
    }
    case WM_EXITSIZEMOVE:
        saveLivePos();      // where the user put the Live window, when he lets go
        return 0;
    // RV4-7: its always-on-top changed by someone else (PowerToys' Always on Top...): the pin and app.txt follow
    case WM_WINDOWPOSCHANGED:
        if (g_liveHwnd == hwnd && realTopmost(hwnd) != g_liveTopmost) {
            g_liveTopmost = !g_liveTopmost;
            saveLivePos();
            if (g_bridge) g_bridge->liveTopmostChanged(g_liveTopmost);
        }
        break;
    case WM_CLOSE:
        closeLiveWindow();
        return 0;
    case WM_PAINT:                   // (fix round, RVB-4: the browser gone -- said in words, like the player's window)
        if (paintNoView(hwnd)) return 0;
        break;
    case WM_DESTROY:
        if (g_bridge) g_bridge->detachLiveView();
        if (g_liveController) { g_liveController->Close(); g_liveController.Reset(); }
        g_liveWebview.Reset();
        g_liveHwnd = nullptr;
        ++g_liveGen;                 // (fix round: a view still on its way is nobody's now)
        g_liveCreating = false;
        return 0;
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The Live view: a second controller on the SAME environment (the player's switches do not hurt a page of text), the
// same fresh no-store serving (the handler is per VIEW: registered here too, or this window would be the one place the
// stale-asset bug could come back), the same lock-down, NO drops, its own handler in the bridge.
// Fix round (RVB-4): each creation has a GENERATION -- a callback that comes back after a rebuild (or the window's end)
// is stale: it closes what it made and touches nothing else (before: a modal error out of nowhere, and g_liveCreating
// reset under the creation that replaced it). A failure is SAID only to the user who asked (`user`: the Live button,
// --live); one of a recovery is tried again, quietly (RVB-3: never a modal, never the foreground, without a gesture).
void createLiveView(bool user) {
    if (!g_environment || !g_liveHwnd || g_liveController || g_liveCreating) return;
    g_liveCreating = true;
    const HWND hwnd = g_liveHwnd;
    const unsigned gen = ++g_liveGen;
    const HRESULT hr = g_environment->CreateCoreWebView2Controller(
        hwnd,
        Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
            [hwnd, gen, user](HRESULT r, ICoreWebView2Controller* controller) -> HRESULT {
                if (gen != g_liveGen) {                       // stale: nothing of it is ours any more
                    if (controller) controller->Close();
                    return S_OK;
                }
                g_liveCreating = false;
                if (FAILED(r) || !controller || g_liveHwnd != hwnd) {   // (failed, or the window went meanwhile)
                    if (controller) controller->Close();
                    if (g_liveHwnd == hwnd && user && !g_quiet)
                        ::MessageBoxW(hwnd, L"SuVtitle could not create the Live window's WebView2 view.", L"SuVtitle Live",
                                      MB_ICONERROR | MB_OK);
                    else if (g_liveHwnd == hwnd && !user && g_hwnd && g_liveViewTries < 5)
                        ::SetTimer(g_hwnd, kLiveViewTimer, 2000u << g_liveViewTries++, nullptr);   // 2, 4 ... 32 s
                    return S_OK;
                }
                g_liveViewTries = 0;
                g_liveController = controller;
                g_liveController->get_CoreWebView2(&g_liveWebview);
                if (!g_liveWebview) { g_liveController->Close(); g_liveController.Reset(); return S_OK; }
                // NO DROPS (AudioPB 1.1.0, review RV5-3): the page has no drop zone, so WebView2's default would show an
                // ACCEPTING cursor and start a file:// navigation only the guard cancels. Refused natively: the no-drop
                // cursor, no navigation at all.
                {
                    ComPtr<ICoreWebView2Controller4> c4;
                    if (SUCCEEDED(g_liveController.As(&c4)) && c4) c4->put_AllowExternalDrop(FALSE);
                }
                g_liveWebview->AddWebResourceRequestedFilter((std::wstring(kWebHost) + L"*").c_str(),
                                                            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                EventRegistrationToken wrToken{};
                g_liveWebview->add_WebResourceRequested(
                    Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                        [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
                            serveFreshWebAsset(a);
                            return S_OK;
                        }).Get(),
                    &wrToken);
                // the same lock-down as the player's (context menu, devtools, SmartScreen, the navigation guard)
                hardenView(g_liveWebview.Get(), &g_liveAllowStringNav);
                watchProcessFailed(g_liveWebview.Get(), /*live=*/true);
                // the bridge: its own handler, the window owning the dialogs it asks for
                if (g_bridge) g_bridge->attachLiveView(g_liveWebview.Get(), hwnd);
                g_liveController->put_IsVisible(::IsWindowVisible(hwnd) ? TRUE : FALSE);
                resizeLiveView();
                // The page is L3's file. If it has not landed, say so in words instead of a blank window.
                const std::wstring page = webContentRoot() + L"\\" + kLivePage;
                if (::GetFileAttributesW(page.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    g_liveAllowStringNav = true;    // the one navigation the guard lets through
                    g_liveWebview->NavigateToString(
                        L"<body style=\"margin:0;background:#14161a;color:#c8ccd4;"
                        L"font:15px system-ui;display:flex;align-items:center;"
                        L"justify-content:center;height:100vh\">"
                        L"<div style=\"text-align:center\"><p>web\\live.html is not "
                        L"installed next to SuVtitle.exe.</p>"
                        L"<p style=\"opacity:.6\">The SuVtitle engine is running; only "
                        L"this page is missing.</p></div></body>");
                } else {
                    g_liveWebview->Navigate((std::wstring(kWebHost) + kLivePage).c_str());
                }
                return S_OK;
            }).Get());
    if (FAILED(hr)) g_liveCreating = false;
}

// A view's renderer died or hangs (ProcessFailed): Reload() -- at most 3 per view per minute; a fourth is OWED (fix round,
// RVB-4), reloaded once the minute allows it: a page that kills its renderer is not reloaded in a loop, and a view is
// never left on Chromium's crash page for good. v: 0 the player's view, 1 the Live window's.
void reloadView(int v, ICoreWebView2* wv) {
    ULONGLONG* r = g_reloads[v ? 1 : 0];
    const ULONGLONG now = ::GetTickCount64();
    if (r[0] != 0 && now - r[0] < 60000) {               // three in the last minute already
        if (!g_reloadOwed[v ? 1 : 0] && g_hwnd) {
            g_reloadOwed[v ? 1 : 0] = true;
            ::SetTimer(g_hwnd, v ? kReloadTimer1 : kReloadTimer0, static_cast<UINT>(60000 - (now - r[0]) + 500), nullptr);
        }
        return;
    }
    r[0] = r[1]; r[1] = r[2]; r[2] = now;
    if (wv) wv->Reload();
}

// A rebuild that FAILED (fix round, RVB-4): what it made is let go, and it is tried again 2, 4, 8 ... 64 s later -- the
// app, the engine and the journal live on (the windows say what is going on: paintNoView).
void rebuildFailed() {
    g_rebuilding = false;
    ++g_envGen;                                           // (what the failed attempt still has on its way is stale)
    g_viewsLost = true;
    if (g_controller) { g_controller->Close(); g_controller.Reset(); }
    g_compController.Reset();                             // (N1: the composition controller went with it)
    g_webview.Reset();
    g_environment.Reset();
    g_rebuildRetry = true;
    const UINT wait = 2000u << (g_rebuildFails < 5 ? g_rebuildFails : 5);
    ++g_rebuildFails;
    if (g_hwnd) {
        ::SetTimer(g_hwnd, kRebuildTimer, wait, nullptr);
        ::InvalidateRect(g_hwnd, nullptr, TRUE);
    }
    if (g_liveHwnd) ::InvalidateRect(g_liveHwnd, nullptr, TRUE);
}

// Open the Live window, or bring it up: created the first time (placed by app.txt LIVEPOS -- or, the very first time, where
// AudioPB's Translator window was -- with its title bar on a monitor), its view created if it has none, shown (never
// activated for a test instance), in front. Before the player's view exists it only notes the request (the player's
// attach opens it: the engine and the environment come from there).
void openLiveWindow() {
    cancelQuit();             // (fix round: asked for while the app was ending -- it stays)
    g_liveRequested = true;
    // (fix round, RVB-4: the browser is gone and its rebuild is OWED -- the user asking for the window is his gesture to
    // try now: the limiter's wait is not his to sit through)
    if (g_viewsLost && !g_rebuilding && g_hwnd) {
        ::KillTimer(g_hwnd, kRebuildTimer);
        g_rebuildRetry = false;
        g_rebuilds[0] = g_rebuilds[1] = g_rebuilds[2] = 0;
        g_rebuilding = true;
        createWebView(g_hwnd);
    }
    if (!g_environment || !g_webview || !g_bridge) return;
    g_liveRequested = false;
    if (!g_liveHwnd) {
        static bool classRegistered = false;
        static std::wstring liveClass;
        if (!classRegistered) {
            liveClass = pblaunch::suffixed(kLiveWindowClass);   // "SuVtitle.LiveWindow[.<tag>]"
            WNDCLASSEXW wc{};
            wc.cbSize        = sizeof(wc);
            wc.lpfnWndProc   = LiveWndProc;
            wc.hInstance     = ::GetModuleHandleW(nullptr);
            wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
            wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            wc.lpszClassName = liveClass.c_str();
            wc.hIcon   = ::LoadIconW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON));
            wc.hIconSm = (HICON)::LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                             ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
            if (!wc.hIcon) wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
            ::RegisterClassExW(&wc);
            classRegistered = true;
        }
        // Where: app.txt's LIVEPOS; else, ONCE, where AudioPB's Translator window was (AudioPBImport.h seedLivePos, read
        // only -- the window writes its LIVEPOS as soon as it is shown, so this never runs again); else 760 x 560 (1.1.0's
        // default). A rectangle whose title bar is on no monitor any more (a second screen unplugged) is brought back
        // onto the nearest one (AppConfig.h clampWinPos): a window the user cannot grab is a window he cannot use.
        pbapp::WinPos pos;
        std::string text;
        const std::wstring f = appTxtPath();
        bool have = !f.empty() && pbtrf::readFileAll(f, text, 1ULL << 20) && pbapp::parseLivePos(text, pos);
        if (!have) have = pbimport::seedLivePos(pbimport::productRoots(), pos);
        if (have) {
            const RECT title{ pos.x + 8, pos.y, pos.x + pos.w - 8, pos.y + 24 };
            if (!::MonitorFromRect(&title, MONITOR_DEFAULTTONULL)) {
                const RECT r{ pos.x, pos.y, pos.x + pos.w, pos.y + pos.h };
                MONITORINFO mi{};
                mi.cbSize = sizeof(mi);
                if (::GetMonitorInfoW(::MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi))
                    pos = pbapp::clampWinPos(pos, mi.rcWork.left, mi.rcWork.top, mi.rcWork.right, mi.rcWork.bottom);
            }
        }
        UINT dpi = 96;
        {
            HMODULE u32 = ::GetModuleHandleW(L"user32.dll");
            auto getDpi = u32 ? (UINT(WINAPI*)())::GetProcAddress(u32, "GetDpiForSystem") : nullptr;
            if (getDpi) dpi = getDpi();
        }
        g_liveTopmost = have && pos.topmost;
        g_liveHwnd = ::CreateWindowExW((g_liveTopmost ? WS_EX_TOPMOST : 0) | (g_quiet ? WS_EX_NOACTIVATE : 0),
                                       liveClass.c_str(), kLiveWindowTitle, WS_OVERLAPPEDWINDOW,
                                       have ? pos.x : CW_USEDEFAULT, have ? pos.y : CW_USEDEFAULT,
                                       have ? pos.w : (int)(760 * dpi / 96), have ? pos.h : (int)(560 * dpi / 96),
                                       nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (!g_liveHwnd) return;
        g_liveTopmost = realTopmost(g_liveHwnd);      // RV4-7: the state its page's first `ready` is told
        g_bridge->liveTopmostChanged(g_liveTopmost);
        ::ShowWindow(g_liveHwnd, g_quiet ? SW_SHOWNOACTIVATE : (have && pos.maximized ? SW_SHOWMAXIMIZED : SW_SHOW));
        saveLivePos();                                // LIVEPOS exists from now on: the seed never runs again
    }
    // (fix round, RVB-4: a view left on Chromium's crash page -- the per-minute limiter refused its reload -- is reloaded
    // NOW: the user asking for the window is the gesture the limiter was waiting for)
    if (g_reloadOwed[1] && g_liveWebview) {
        ::KillTimer(g_hwnd, kReloadTimer1);
        g_reloadOwed[1] = false;
        g_reloads[1][0] = g_reloads[1][1] = g_reloads[1][2] = 0;
        reloadView(1, g_liveWebview.Get());
    }
    g_liveViewTries = 0;
    createLiveView(/*user=*/true);                    // (none yet -- or rebuilt after the browser process died)
    if (::IsIconic(g_liveHwnd)) ::ShowWindow(g_liveHwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_RESTORE);
    else if (!::IsWindowVisible(g_liveHwnd)) ::ShowWindow(g_liveHwnd, g_quiet ? SW_SHOWNOACTIVATE : SW_SHOW);
    if (g_liveController) g_liveController->put_IsVisible(TRUE);
    if (!g_quiet) ::SetForegroundWindow(g_liveHwnd);
}

// ProcessFailed (LIVE_PLAN 4.3, BOTH views): SuVtitle had none (the study 3.1), and a meeting of two hours must not end on
// a frozen window -- the engine and the journal go on without the view, the view must come back to them.
//   * the view's RENDERER died or hangs: Reload() -- the page's `ready` gets everything back (the player its session,
//     the Live window its state and its last 200 lines). At most 3 per view per minute: a page that kills its renderer
//     every time must not be reloaded in a loop.
//   * the BROWSER process died: every view of the environment is gone -- rebuilt (posted, once, rebuildViews).
// Other kinds (a GPU or utility process: Chromium restarts those itself) need nothing from us.
void watchProcessFailed(ICoreWebView2* wv, bool live) {
    if (!wv) return;
    EventRegistrationToken t{};
    wv->add_ProcessFailed(
        Callback<ICoreWebView2ProcessFailedEventHandler>(
            [live, egen = g_envGen](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* a) -> HRESULT {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
                if (!a || FAILED(a->get_ProcessFailedKind(&kind))) return S_OK;
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
                    if (g_hwnd) ::PostMessageW(g_hwnd, kWmRebuildViews, egen, 0);   // (with ITS generation: fix round)
                    return S_OK;
                }
                if (kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
                    && kind != COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) return S_OK;
                reloadView(live ? 1 : 0, sender);            // (3 per view per minute; a fourth OWED: fix round, RVB-4)
                return S_OK;
            }).Get(),
        &t);
}

// The browser process died: every controller of the environment is dead. Both views are closed, the environment made
// again (createWebView: the player's view first -- the bridge attaches it again: detachView made room), and the Live
// window's view after it, if the Live window was up. At most 3 rebuilds in 5 minutes. The engine, the
// journal and the player's state never stopped: they live in this process, not in the browser's.
// FIX ROUND: the Live window gets its VIEW back (g_liveViewOwed -> createLiveView), never openLiveWindow's show / restore /
// foreground (RVB-3: a minimized, pinned Live window stays minimized over the user's game); the dead views are let go AT
// ONCE (the windows say so: paintNoView), and a rebuild the limiter refuses is OWED -- a timer, never dropped (RVB-4).
void rebuildViews(unsigned gen) {
    if (!g_hwnd || gen != g_envGen) return;                  // (a notice about views already let go: the other view's)
    if (g_environment || g_webview) {                         // the FIRST notice of this death: let go of what died
        ++g_envGen;                                           // every creation of the dead generation is stale from now on
        if (g_liveHwnd && ::IsWindowVisible(g_liveHwnd)) g_liveViewOwed = true;
        if (g_bridge) { g_bridge->detachLiveView(); g_bridge->detachView(); }
        if (g_liveController) { g_liveController->Close(); g_liveController.Reset(); }
        g_liveWebview.Reset();
        g_liveCreating = false;
        ++g_liveGen;                                          // a Live view still on its way is stale from now on
        if (g_controller) { g_controller->Close(); g_controller.Reset(); }
        g_compController.Reset();                             // (N1: the composition controller went with it)
        // (N1) the page that drove mpv is gone: the film waits, paused and out of sight (the notice is painted where it
        // was), until the rebuilt page places it again
        if (g_native) { g_native->pause(); g_native->place(RECT{}); }
        g_webview.Reset();
        g_environment.Reset();
        g_viewsLost = true;
        ::InvalidateRect(g_hwnd, nullptr, TRUE);
        if (g_liveHwnd) ::InvalidateRect(g_liveHwnd, nullptr, TRUE);
    } else if (g_rebuilding || !g_viewsLost) {
        return;                                               // (the second view's notice of the same death: on its way)
    }
    const ULONGLONG now = ::GetTickCount64();
    if (g_rebuilds[0] != 0 && now - g_rebuilds[0] < 300000) {   // three in five minutes: this one is OWED
        g_rebuildRetry = false;
        ::SetTimer(g_hwnd, kRebuildTimer, static_cast<UINT>(300000 - (now - g_rebuilds[0]) + 1000), nullptr);
        return;
    }
    g_rebuilds[0] = g_rebuilds[1]; g_rebuilds[1] = g_rebuilds[2]; g_rebuilds[2] = now;
    g_rebuilding = true;
    g_rebuildFails = 0;
    createWebView(g_hwnd);
}

} // namespace

// ----------------------------------------------------------------------------
// Entry point
// ----------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nCmdShow) {
    // 1.1.0 FIX ROUND (N1, docs\REVIEW_RV-NH.md MEDIUM4): the DLL search path LOCKED before anything is loaded by name --
    // the exe's folder, System32 and <exe dir>\mpv\ only, never the current directory (Explorer starts SuVtitle with --play
    // in the FILM's folder; libmpv loads DLLs by name that most PCs lack). MpvPlayer.h lockDllSearch.
    g_dllLocked = suvmpv::MpvPlayer::lockDllSearch();
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
        // The hand-over to the running SuVtitle: 0 = it took the request, 2 = it did not.
        const auto handOver = [&]() -> int {
        // SUVTITLE LIVE (1.1.0): started by the "SuVtitle Live" shortcut (--live): the SuVtitle already running opens its
        // Live window (WM_COPYDATA kCopyLive, nothing in it), in front -- this process passes it its right to the
        // foreground, never a test instance's. With --play too, the paths follow below.
        bool liveSent = true;
        if (args.live) {
            liveSent = false;
            if (existing) {
                COPYDATASTRUCT lcd{};
                lcd.dwData = pblaunch::kCopyLive;     // (cbData 0, lpData null: the request IS the tag)
                DWORD livePid = 0;
                ::GetWindowThreadProcessId(existing, &livePid);
                if (livePid && !testInstance) ::AllowSetForegroundWindow(livePid);
                DWORD_PTR lres = 0;
                liveSent = ::SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&lcd),
                                                 SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &lres) != 0 && lres == TRUE;
            }
            if (!args.play) return liveSent ? 0 : 2;
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
            sent = sent && liveSent;
            return sent ? 0 : 2;
        }
        // SUVTITLE LIVE (1.1.0): the running SuVtitle's player is HIDDEN (it was started by --live, or its player was
        // closed while the Live window stays): a plain launch means "SuVtitle" -- the player -- asked for as an EMPTY
        // --play (the same path: kWmPlayPending -> bringUp -> the player, nothing added to its queue).
        if (existing && !::IsWindowVisible(existing)) {
            std::vector<wchar_t> none = pblaunch::packPlay({});
            COPYDATASTRUCT ecd{};
            ecd.dwData = pblaunch::kCopyPlay;
            ecd.cbData = static_cast<DWORD>(none.size() * sizeof(wchar_t));
            ecd.lpData = none.data();
            DWORD hiddenPid = 0;
            ::GetWindowThreadProcessId(existing, &hiddenPid);
            if (hiddenPid && !testInstance) ::AllowSetForegroundWindow(hiddenPid);
            DWORD_PTR eres = 0;
            const bool shownBack = ::SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&ecd),
                                                         SMTO_ABORTIFHUNG | SMTO_BLOCK, 10000, &eres) != 0 && eres == TRUE;
            return shownBack ? 0 : 2;
        }
        if (existing && !testInstance) {
            // not ShowWindow(SW_RESTORE) on a window that is not minimised: it would un-maximise it
            if (::IsIconic(existing)) ::ShowWindow(existing, SW_RESTORE);
            ::SetForegroundWindow(existing);
        }
        return existing ? 0 : 2;
        };
        const int handed = handOver();
        // SUVTITLE LIVE fix round (the ADDENDUM: "a relaunch during that wait must behave"): the running SuVtitle may be
        // ENDING -- its last window closed, it waited for the live drain and is shutting down -- or gone: its window
        // vanished before it could take this request. A launch is not lost to that race: this process waits for the
        // single-instance mutex to be let go (the old instance's end), then IS the app. A window that is THERE but did
        // not answer (hung) keeps the old rule: this launch gives up.
        if (handed == 0 || (existing && ::IsWindow(existing))) { ::CloseHandle(hSingle); return handed; }
        const DWORD took = ::WaitForSingleObject(hSingle, 30000);
        if (took != WAIT_OBJECT_0 && took != WAIT_ABANDONED) { ::CloseHandle(hSingle); return 2; }
    }

    // WebView2 + the shell's dialogs use apartment-threaded COM -- OLE on top of it (1.1.0, N1: the window's drop target,
    // RegisterDragDrop, needs OleInitialize; it initialises the same apartment).
    if (FAILED(::OleInitialize(nullptr))) { ::CloseHandle(hSingle); return 1; }
    // (N1) a TEST INSTANCE may ask for no TME_LEAVE (SUVTITLE_TEST_NO_MOUSELEAVE=1): its moves are POSTED while the user's
    // real cursor is elsewhere, and a leave would follow each at once (forwardMouse). Never read by the user's SuVtitle.
    if (g_quiet) {
        wchar_t v[4] = {};
        g_noTrackLeave = ::GetEnvironmentVariableW(L"SUVTITLE_TEST_NO_MOUSELEAVE", v, 4) == 1 && v[0] == L'1';
    }

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
    // (N1) CS_DBLCLKS: a visual-hosted view gets its double-clicks from THIS window (forwardMouse) -- without the style
    // Windows never makes one, and a double-click on the picture would not reach the page (its full screen)
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = ::LoadCursorW(nullptr, IDC_ARROW);
    // (N1) BLACK: under a transparent page, where no picture is (a sound file, nothing open yet), the window itself shows
    wc.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
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
                               mainClass.c_str(), kWindowTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,   // (N1: GDI never paints over the picture)
                               haveSaved ? pos.x : CW_USEDEFAULT, haveSaved ? pos.y : CW_USEDEFAULT,
                               haveSaved ? pos.w : (int)(1100 * dpi / 96), haveSaved ? pos.h : (int)(720 * dpi / 96),
                               nullptr, nullptr, hInst, nullptr);
    if (!g_hwnd) { g_bridge.reset(); ::OleUninitialize(); ::CloseHandle(hSingle); return 1; }
    g_topmost = realTopmost(g_hwnd);                 // RV4-7: the state the page's first `ready` is told
    g_bridge->translatorTopmostChanged(g_topmost);

    // WG: started by Explorer to PLAY a file: the player comes up with it (once the view exists: kWmPlayPending).
    if (args.play) { g_pendingPlay = args.paths; g_playRequested = true; g_playSources = 1; }
    // A test instance: shown, never activated (WS_EX_NOACTIVATE + SW_SHOWNOACTIVATE), never maximised by a saved state.
    int show = nCmdShow;
    if (g_quiet) show = SW_SHOWNOACTIVATE;
    else if (haveSaved && pos.maximized && (show == SW_SHOWNORMAL || show == SW_SHOWDEFAULT || show == SW_SHOW)) show = SW_SHOWMAXIMIZED;
    // SUVTITLE LIVE (1.1.0): --live opens the Live window ALONE -- "the taskbar shows only it" (LIVE_PLAN 4.3). The
    // player's window above is the app's hub all the same (the 1 Hz tick, the engine's wake, WM_COPYDATA, the
    // single-instance target, the WebView2 environment); it is simply never shown -- a hidden window has no taskbar
    // button -- until the user asks for the player (a plain launch, --play). --live --play shows both.
    if (args.live) g_liveRequested = true;            // opened once the player's view exists (createWebViewEnv)
    if (args.live && !args.play) {
        g_showMaxOnce = !g_quiet && haveSaved && pos.maximized;
    } else {
        ::ShowWindow(g_hwnd, show);
        ::UpdateWindow(g_hwnd);
    }

    MSG msg;
    while (::GetMessageW(&msg, nullptr, 0, 0)) {
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    g_bridge.reset();          // (already shut down in WM_DESTROY; its destructor is a no-op then)
    g_native.reset();          // (N1: AFTER the bridge -- its sink captured it; mpv itself ended in WM_DESTROY)
    g_liveController.Reset();  // (SUVTITLE LIVE: closed with its window, in WM_DESTROY)
    g_liveWebview.Reset();
    g_compController.Reset();
    g_controller.Reset();
    g_webview.Reset();
    g_environment.Reset();
    ::OleUninitialize();
    ::CloseHandle(hSingle);
    return static_cast<int>(msg.wParam);
}
